# Модули: как устроены и как добавлять новые

## Тип модуля vs экземпляр

**Тип** компилируется в прошивку и живёт в `ModuleRegistry`.
**Экземпляр** создаёт пользователь через браузер, он живёт в конфигурации.
Пользователь никогда не загружает бинарные плагины (§50) — но и никогда не
перепрошивает плату, чтобы добавить второй HX711.

## Каталог

| id | Категория | Статус | Заметки |
|---|---|---|---|
| `sim_signal` | sensor | ✅ M0 | sine / ramp / square / triangle / const / random walk |
| `hx711` | sensor | ✅ M2 | публикует сырые отсчёты; в физическую величину переводит калибровка |
| `aht20` | sensor | ✅ M2 | I²C 0x38, CRC-8, конверсия 80 мс без блокировки |
| `bmp280` | sensor | ✅ M2 | I²C 0x76/0x77, проверка chip ID, целочисленная компенсация |
| `analog_in` | sensor | ✅ M2 | только ADC1; усреднение; честно сообщает об отсутствии калибровки АЦП |
| `digital_in` | sensor | ✅ M2 | антидребезг, инверсия, внутренние подтяжки |
| `calibration` | processing | ✅ M0 | polynomial / piecewise table |
| `moving_average` | processing | ✅ M0 | O(1), окно до 64 |
| `ds18b20` | sensor | M2.1 | 1-Wire на RMT: битбэнг из кооперативной задачи ненадёжен |
| `relay`, `pwm`, `digital_out`, `heater`, `fan`, `servo` | output | M7 | |
| `median`, `lowpass`, `derivative`, `integral`, `statistics`, `clamp`, `deadband` | processing | M5 |
| `formula` | virtual | M5 |
| `pid`, `thermostat`, `threshold`, `timer` | control | M8 |

## Манифест

`ModuleManifest` — `static constexpr` во flash. Он описывает модуль настолько полно,
что frontend строит по нему форму целиком, а `DeviceManager` — валидирует
конфигурацию до создания драйвера. Структура — в `core/ModuleManifest.h`,
JSON-проекция и пример — в `docs/architecture.md` §8.

Поля `ParamSpec`, на которые стоит обратить внимание:

| Поле | Зачем |
|---|---|
| `type: gpio` + `pin_use` | UI показывает только пины, физически способные на такую роль; занятые — с именем владельца |
| `visible_if: "waveform!=constant"` | простые условия видимости без кода в UI |
| `advanced: true` | прячется за «Advanced» — §60, пользователь не обязан знать всё сразу |
| `default` | значения по умолчанию живут **только** здесь и не дублируются в конфиге |
| `min` / `max` / `step` | одна и та же проверка в браузере и в прошивке |

`ChannelSpec` описывает каналы, которые модуль создаёт. Их создаёт `DeviceManager`,
а не драйвер — поэтому имена, единицы и диапазоны гарантированно соответствуют
манифесту.

## Как добавить драйвер датчика

Пример: `BMP280` (I²C, температура + давление).

**1. Заголовок** `src/modules/sensors/Bmp280Driver.h`

```cpp
class Bmp280Driver final : public IDevice {
 public:
  static const ModuleManifest& manifest();
  static IDevice* create() { return new Bmp280Driver(); }

  Status configure(const DeviceContext& ctx) override;
  Status begin() override;
  void   poll(Micros now) override;
  void   end() override;
  DeviceState  state()     const override { return state_; }
  const Error& lastError() const override { return lastError_; }
  Status selfTest() override;
 private:
  DeviceContext ctx_{};
  DeviceState state_ = DeviceState::kDisabled;
  Error lastError_{};
  std::uint8_t bus_ = 0, address_ = 0x76;
  Micros conversionDueUs_ = 0;
  bool waitingForConversion_ = false;
  /* калибровочные регистры чипа */
};
```

**2. Манифест** — в анонимном namespace в `.cpp`:

```cpp
constexpr ParamSpec kParams[] = {
  ParamSpec{"bus", "I2C bus", ParamType::kBusRef, nullptr, nullptr,
            0, 1, 1, "0", nullptr, 0, PinUse::kBusSignal, true, false, nullptr},
  ParamSpec{"address", "I2C address", ParamType::kI2cAddress, nullptr,
            "0x76 or 0x77 depending on the SDO pin",
            0x76, 0x77, 1, "0x76", nullptr, 0, PinUse::kBusSignal, true, false, nullptr},
  ParamSpec{"oversampling", "Oversampling", ParamType::kSelect, nullptr, nullptr,
            0, 0, 0, "x4", kOversamplingOptions, 4,
            PinUse::kBusSignal, false, true, nullptr},
};

constexpr ChannelSpec kChannels[] = {
  ChannelSpec{"temperature", "Temperature", "degC", "temperature",
              ChannelDirection::kInput, -40.0f, 85.0f,  2, true},
  ChannelSpec{"pressure",    "Pressure",    "Pa",   "pressure",
              ChannelDirection::kInput, 30000.0f, 110000.0f, 0, true},
};
```

**3. `configure()`** — только захват ресурсов, без обмена по шине:

```cpp
Status Bmp280Driver::configure(const DeviceContext& ctx) {
  ctx_ = ctx;
  bus_     = static_cast<std::uint8_t>(ctx.config->getInt("bus", 0));
  address_ = static_cast<std::uint8_t>(ctx.config->getInt("address", 0x76));
  // Адрес на конкретной шине — такой же ресурс, как пин.
  return ctx.resources->claim(i2cAddressResource(bus_, address_), ctx.self, "BMP280");
}
```

**4. `begin()`** — проба и чтение калибровки чипа; при отсутствии ответа
возвращает `kDeviceNotResponding`, и UI покажет ровно `I2C device 0x76 not found`.

**5. `poll()` — обязательно неблокирующий:**

```cpp
void Bmp280Driver::poll(Micros now) {
  if (state_ != DeviceState::kRunning) return;

  if (!waitingForConversion_) {
    startConversion();                        // одна короткая транзакция
    conversionDueUs_ = now + conversionTimeUs_;
    waitingForConversion_ = true;
    return;                                   // НЕ ждём здесь
  }
  if (now < conversionDueUs_) return;         // ещё не готово — выходим

  RawSample raw;
  if (!readRaw(raw)) { fault(ErrorCode::kDeviceCrcError); return; }
  waitingForConversion_ = false;

  ctx_.channels->publishRaw(ctx_.channelHandles[0], compensateT(raw), now);
  ctx_.channels->publishRaw(ctx_.channelHandles[1], compensateP(raw), now);
}
```

**6. Регистрация** — две строки в `modules/BuiltinModules.cpp`:

```cpp
registry.add(ModuleDescriptor{&Bmp280Driver::manifest(),
                              &Bmp280Driver::create, nullptr, nullptr});
```

**7. Тесты.** Логика компенсации BMP280 — чистая арифметика над регистрами:
выносим её в свободные функции и проверяем на эталонных значениях из даташита
в `test/test_modules/`. Обмен по шине проверяем в аппаратном режиме через `selfTest()`.

**Изменений во frontend: ноль.**

## Как драйверы тестируются без железа

Драйвер видит `II2cBus` / `IGpioPort` / `IAdcPort`, а не `Wire` и не
`digitalWrite`. Поэтому в тесте ему подсовывают фальшивую шину и проверяют
ровно те ветки, которые на живом стенде не воспроизвести (ADR-0011):

```cpp
test::FakeI2cBus bus;
bus.attach(0x38);                        // датчик на месте
bus.queueRead(0x38, {0x1C});             // status: откалиброван, не занят
bus.queueRead(0x38, aht20Frame(...));    // кадр измерения
bus.failNext(3);                         // а теперь три NACK подряд
```

Что это ловит на практике:

* «датчика нет» → `begin()` возвращает `DEVICE_NOT_RESPONDING`, а не зависает;
* «одна ошибка — WARNING, три подряд — ERROR»;
* HX711 при оторванном проводе не тратит **ни одной микросекунды** ожидания
  (тест сравнивает счётчик `delayMicros` до и после);
* BMP280 не публикует значение сброса `0x80000` (иначе на дашборде −145 °C);
* BME280, воткнутый по адресу BMP280, распознаётся по chip ID и называется по имени;
* целочисленная компенсация BMP280 сверяется с независимой реализацией в `double`
  по всему диапазону АЦП: две структурно разные формулы сходятся в пределах
  0.02 °C и 2 Па.

Чистая арифметика вынесена в классы без состояния (`Aht20Protocol`,
`Bmp280Protocol`, `Hx711Protocol`), чтобы её можно было проверять отдельно —
например, CRC-8 AHT20 сверяется с каталожным значением CRC-8/NRSC-5 (`0xF7`
для строки `123456789`), то есть с внешним эталоном, а не с самим собой.

## Правила для драйверов

1. `poll()` не блокирует. Никаких `delay()`, `while (!ready)`, ожиданий дольше
   нескольких микросекунд. Нужна пауза — запомните дедлайн и выйдите.
2. Ресурсы — только через `ctx.resources`. Прямой `pinMode()` по пину, который никто
   не захватил, — это то, ради чего существует `ResourceManager`.
3. Каналы не создаются драйвером. Они приходят готовыми в `ctx.channelHandles`.
4. Откат при ошибке — не ваша забота: `DeviceManager` вызовет `releaseAllOwnedBy()`.
5. Никаких `String` и аллокаций в `poll()`.
6. Ошибка = `ErrorCode` + внятный `detail`. «Device error» без подробностей —
   нарушение §46.
7. Драйвер не знает ни про дашборды, ни про эксперименты, ни про WebSocket.

## Процессоры

`IProcessor` строго одновходовой. Если преобразованию нужны два канала
(компенсация, ΔT, мощность) — это **виртуальный канал**, а не процессор.
Ограничение сознательное: линейный конвейер можно показать пользователю
и отладить, произвольный граф внутри канала — нет.

## Контроллеры

`IController` (PID, термостат, порог, таймер) читает входной канал и пишет в
выходной. Он не знает типа датчика и типа исполнительного устройства (§28).
Замена термопары на пирометр не требует ничего, кроме смены `input` в конфигурации PID.
