# Архитектура — Universal ESP32 Laboratory Controller

**Статус:** Milestone 0–4 завершены. Ядро, менеджер устройств, конвейер обработки,
слой конфигурации, аппаратные драйверы, REST API и WebSocket-телеметрия
реализованы и покрыты тестами (**106 тестов, 0 падений**, чисто под ASan/UBSan).
Полный цикл «подключил датчик → нашёл сканером → добавил → увидел значения»
выполняется в браузере, без консоли и без curl.
**Версия схемы конфигурации:** 1
**Целевая платформа:** ESP32 DevKit (WROOM-32), сборка также для ESP32-S3.

---

## 0. Одно предложение

Прошивка — это **исполняющая среда**: ядро, реестр типов модулей и набор менеджеров;
конкретная лабораторная установка — это **данные** (конфигурация в LittleFS), которые
пользователь создаёт через браузер, а не код, который кто-то компилирует.

Всё остальное в этом документе — следствие этого предложения.

---

## 1. Слои и поток данных

```
┌────────────────────────────────────────────────────────────────────────────┐
│  Browser (Svelte SPA, отдаётся из LittleFS в .gz)                          │
└───────────────┬─────────────────────────────────────┬──────────────────────┘
                │ REST /api/v1/**  (конфигурация)     │ WS /ws/live (телеметрия)
┌───────────────▼─────────────────────────────────────▼──────────────────────┐
│  api/         RestApi   TelemetryBatcher   Serializers   PathRouter        │
├────────────────────────────────────────────────────────────────────────────┤
│  app/         SystemManager (порядок старта, safe mode)  BootCounter       │
├────────────────────────────────────────────────────────────────────────────┤
│  services/    DeviceManager   ChannelManager   CalibrationManager          │
│               ProcessingManager  RuleEngine    ExperimentManager           │
│               DataLogger      DashboardManager ProfileManager              │
├────────────────────────────────────────────────────────────────────────────┤
│  modules/     sensors/  outputs/  processing/  control/  virtual/          │
│               (реализуют IDevice / IProcessor / IController)               │
├────────────────────────────────────────────────────────────────────────────┤
│  core/        ModuleRegistry  ResourceManager  Scheduler  EventBus         │
│               ChipProfile     Error/Result     Clock      SafetyManager    │
├────────────────────────────────────────────────────────────────────────────┤
│  buses/       I2CManager  SPIManager  UARTManager  GPIOManager  ADCManager │
├────────────────────────────────────────────────────────────────────────────┤
│  platform/    esp32/ (Arduino + IDF)          host/ (для unit-тестов)      │
├────────────────────────────────────────────────────────────────────────────┤
│  storage/     ConfigStorage (LittleFS, JSON+версия)   NvsStore   SdCard    │
└────────────────────────────────────────────────────────────────────────────┘
```

Поток измерения — строго в одну сторону:

```
Hardware → Driver(IDevice) → publishRaw(handle, value)
        → ChannelManager → Processing pipeline (calibration → filters → compensation)
        → Channel {raw, calibrated, processed, quality, timestamp}
        → слушатели: DataLogger | RuleEngine | VirtualChannels | WsBatcher | PID
        → WebSocket → Dashboard
```

Dashboard **никогда** не знает, из какого GPIO пришло число. Он знает `ChannelHandle`.

### Отличия от структуры из ТЗ (§4) и зачем

| Изменение | Причина |
|---|---|
| Добавлен слой `platform/` (`esp32/`, `host/`) | Ядро и services собираются host-компилятором → `pio test -e native` гоняет реальные unit-тесты Scheduler/ResourceManager/ChannelManager/калибровки без железа. Это даёт CI и резко ускоряет разработку. |
| Добавлен `core/ChipProfile` | Карта пинов как **данные**, а не `#ifdef` по драйверам. Позволяет ResourceManager сказать «GPIO34 не имеет выходного драйвера», а UI — сразу погасить пин. Поддержка нового чипа = новая таблица. |
| `core/Error` + `Result<T>` вынесены в отдельный слой | Требование §46 «никаких немых ошибок» надо чем-то обеспечить структурно. |
| `services/ProcessingManager` выделен из ChannelManager | ChannelManager хранит и раздаёт данные; кто владеет объектами-фильтрами — отдельная забота. |
| `services/ProfileManager` добавлен | §36 (профили установки) — это не то же самое, что ConfigStorage. |
| `core/ConfigView` | Драйверы не видят ArduinoJson. Тестируемость + возможность сменить JSON-библиотеку. |
| Добавлен слой `app/` (M1) | `services/` не должен знать про JSON и файловую систему, `storage/` — про порядок загрузки. Знать и то и другое разрешено ровно одному месту. `main.cpp` знает только про `app/`. |
| `storage/IStorageBackend` (M1) | Позволяет прогнать «обрыв питания при записи», «файл из будущего», «нет места» как обычные unit-тесты. См. ADR-0009. |

---

## 2. Список компонентов

### core/ (не зависит от Arduino, тестируется на хосте)

| Компонент | Ответственность | Статус |
|---|---|---|
| `Types.h` | `FixedString`, handles, `DeviceState`, `ChannelQuality`, `Geometry`, лимиты | ✅ M0 |
| `Error.h/.cpp` | `ErrorCode`, стабильные символы ошибок, `Result<T>`, `Status` | ✅ M0 |
| `Clock.h` | `IClock`, `ManualClock` для тестов | ✅ M0 |
| `ChipProfile` | таблица возможностей GPIO/ADC/шин по чипам | ✅ M0 |
| `ResourceManager` | владение GPIO/ADC/I²C-адресами/SPI/UART/PWM, валидация | ✅ M0 |
| `Scheduler` | кооперативные периодические задачи, приоритеты, бюджет, статистика | ✅ M0 |
| `EventBus` | control-plane события, синхронные + отложенные | ✅ M0 |
| `ModuleManifest` | self-describing schema модуля | ✅ M0 |
| `IModule.h` | `IDevice`, `IOutputDevice`, `IProcessor`, `IController`, `DeviceContext` | ✅ M0 |
| `ModuleRegistry` | каталог типов модулей + фабрики | ✅ M0 |
| `SafetyManager` | жёсткие интерлоки поверх пользовательской автоматики | M8 |

### app/

| Компонент | Ответственность | Статус |
|---|---|---|
| `SystemManager` | последовательность старта, safe mode, главный цикл, перезагрузка конфигурации | ✅ M1 |
| `BootCounter` | счётчик неудачных загрузок (NVS на плате, in-memory в тестах) | ✅ M1 |

### storage/

| Компонент | Ответственность | Статус |
|---|---|---|
| `IStorageBackend` | контракт файловых операций, включая атомарную запись | ✅ M1 |
| `LittleFsBackend` / `PosixBackend` | реализации для платы и для хост-тестов | ✅ M1 |
| `JsonConfigView` | `IConfigView` поверх ArduinoJson (единственная точка стыка) | ✅ M1 |
| `ConfigStorage` | секции, `schemaVersion`, цепочка миграций, revision | ✅ M1 |
| `ConfigApplier` | документ → живая установка и обратно (read-only проекция) | ✅ M1 |
| `SdCardLogSink` | большие логи экспериментов | M10 |

### services/

| Компонент | Ответственность | Статус |
|---|---|---|
| `ChannelManager` | реестр каналов, raw/calibrated/processed, pipeline, слушатели, staleness | ✅ M0 |
| `CalibrationSolver` | МНК-подгонка коэффициентов по эталонным точкам | ✅ M0 |
| `DeviceManager` | жизненный цикл Device, валидация по манифесту, полный откат ресурсов | ✅ M1 |
| `ProcessingManager` | владение экземплярами `IProcessor`, порядок цепочки, привязка к каналам | ✅ M1 |
| `CalibrationManager` | хранение калибровок, версии, привязка к каналам | M5 |
| `RuleEngine` | правила IF/AND/OR с гистерезисом и задержками | M8 |
| `ExperimentManager` | декларативные сценарии, шаги, состояние | M9 |
| `DataLogger` | выбор каналов, частота, CSV, ротация, SD | M10 |
| `DashboardManager` | декларативные дашборды и виджеты | M6 |
| `ProfileManager` | профили установки, переключение без перепрошивки | M11 |

### api/ (реализовано в M3)

| Компонент | Ответственность | Статус |
|---|---|---|
| `ApiRequest` / `ApiResponse` | модель запроса и ответа без HTTP-библиотеки | ✅ M3 |
| `PathRouter` | разбор пути на сегменты, percent-decoding, без аллокаций | ✅ M3 |
| `RestApi` | все маршруты v1, `?dry_run=1`, запись через файл с откатом | ✅ M3 |
| `Serializers` | манифест → форма, карта GPIO, каналы, статистика планировщика | ✅ M3 |
| `TelemetryBatcher` | батчинг по времени, подписка, отбрасывание кадра | ✅ M3 |
| `SystemMetrics` | куча, ФС, сеть — за интерфейсом, чтобы диагностика тестировалась | ✅ M3 |
| `PsychicHttpAdapter` | перевод PsychicHttp ↔ RestApi, отдача SPA в `.gz` | ✅ M3 (не покрыт хост-тестами) |
| `AuthGuard`, сессии | | M11 |

### buses/ (реализовано в M2)

| Компонент | Ответственность | Статус |
|---|---|---|
| `II2cBus` / `IGpioPort` / `IAdcPort` / `IBusProvider` | контракты доступа к железу, без Arduino | ✅ M2 |
| `IBusConfigurator` | настройка шин; отделена от их использования | ✅ M2 |
| `I2cScanner` | сканирование адресов + осторожные подсказки по модулю | ✅ M2 |
| `WireI2cBus`, `Esp32BusProvider` | реализации на ESP32, проверка владения пином | ✅ M2 |
| `SpiManager`, `UartManager`, 1-Wire на RMT | | M2.1 / M7 |

### modules/

| Модуль | Категория | Статус |
|---|---|---|
| `sim_signal` | sensor | ✅ M0 |
| `calibration`, `moving_average` | processing | ✅ M0 |
| `hx711`, `aht20`, `bmp280`, `analog_in`, `digital_in` | sensor | ✅ M2 |
| `ds18b20` | sensor | M2.1 (1-Wire на RMT) |

---

## 3. C++-модель основных интерфейсов

Полные объявления — в исходниках; здесь суть.

```cpp
// Что модуль может трогать. Ничего сверх этого.
struct DeviceContext {
  DeviceHandle          self;
  const ModuleManifest* manifest;
  const IConfigView*    config;        // не ArduinoJson!
  const IClock*         clock;
  ResourceManager*      resources;
  ChannelManager*       channels;
  EventBus*             events;
  const ChannelHandle*  channelHandles;  // созданы DeviceManager по манифесту
  std::uint8_t          channelCount;
};

class IDevice {
 public:
  virtual Status configure(const DeviceContext&) = 0;  // захват ресурсов, без I/O
  virtual Status begin()                          = 0;  // может вернуть kTimeout → повтор
  virtual void   poll(Micros now)                 = 0;  // НЕБЛОКИРУЮЩИЙ
  virtual void   end()                            = 0;
  virtual DeviceState  state()      const = 0;
  virtual const Error& lastError()  const = 0;
  virtual Status selfTest() { return fail(ErrorCode::kNotSupported); }
};

class IOutputDevice : public IDevice {
 public:
  virtual Status write(ChannelHandle, float value) = 0;
  virtual void   failSafe()                        = 0;  // вызывается SafetyManager
};

class IProcessor {
 public:
  virtual const char* typeId() const = 0;
  virtual Status configure(const IConfigView&) = 0;
  virtual float  process(float input, Micros now, bool& valid) = 0;
  virtual void   reset() = 0;
};

class IController {
 public:
  virtual const char* typeId() const = 0;
  virtual Status configure(const IConfigView&, ChannelManager&) = 0;
  virtual void   update(Micros now) = 0;
  virtual void   setEnabled(bool) = 0;
  virtual bool   enabled() const = 0;
};
```

**Ключевые решения, зафиксированные в этих подписях**

1. `poll()` принимает `now` — драйвер не читает часы сам, значит его можно
   прогнать через 6 часов виртуального времени за миллисекунду в тесте.
2. Драйвер не создаёт каналы. Их создаёт `DeviceManager` по `ChannelSpec` из
   манифеста и передаёт handles. Значит имена/единицы/диапазоны каналов
   гарантированно соответствуют манифесту, а не тому, что придумал драйвер.
3. Драйвер не освобождает ресурсы сам. Освобождает `DeviceManager` через
   `resources.releaseAllOwnedBy(self)` — единственный корректный способ откатить
   частично выполненную серию `claim()`.
4. `IProcessor` — строго 1 вход / 1 выход. Многовходовые преобразования
   (компенсация, ΔT, мощность) — это **виртуальные каналы**, не процессоры.
   Иначе pipeline перестаёт быть линейным и его нельзя ни отобразить, ни отладить.

---

## 4. Модель Device / Module / Channel

```
ModuleRegistry (типы, компилируются в прошивку)
└── ModuleDescriptor
    ├── ModuleManifest  (id, category, ParamSpec[], ChannelSpec[], лимиты)
    └── factory: IDevice* / IProcessor* / IController*
                    │
                    │  пользователь: POST /api/v1/devices
                    ▼
DeviceManager (экземпляры, живут в конфигурации)
└── DeviceRecord { handle, key "hx711_01", manifest, instance, state, geometry }
        │  создаёт по одному каналу на каждый ChannelSpec
        ▼
ChannelManager
└── Channel { handle, key "mass_01", unit, quantity, source=DeviceHandle,
              raw / calibrated / processed, quality, ts, geometry, pipeline }
        │
        ├── ProcessingManager: IProcessor[] (calibration → median → MA → …)
        ├── VirtualChannels:   formula(ΔT = T_center − T_edge)
        ├── Controllers:       PID(input=chan, output=chan)
        ├── DataLogger:        подписка на подмножество каналов
        └── WebSocket batcher: подписка на подмножество каналов
```

Пример: `AHT20 #1` → манифест объявляет два `ChannelSpec` (`temperature`, `humidity`)
→ DeviceManager создаёт каналы `aht20_01.temperature` и `aht20_01.humidity`
→ драйвер публикует в `channelHandles[0]` и `channelHandles[1]`.
Один тип драйвера, два экземпляра, четыре канала — без единой строки специального кода.

### Идентификаторы

| Уровень | Тип | Пример | Где используется |
|---|---|---|---|
| Тип модуля | `const char*` | `"hx711"` | манифест, конфигурация, REST |
| Экземпляр устройства | `KeyString` + `DeviceHandle` | `"hx711_01"` / `7` | REST/конфиг ↔ горячий путь |
| Канал | `KeyString` + `ChannelHandle` | `"mass_01"` / `12` | REST/формулы ↔ горячий путь |

Строки живут только в дескрипторах и на границе API. Внутри — `uint16`.
Это не микрооптимизация: 20 каналов × 80 Гц × сравнение строк — это заметная доля
кадра на 240 МГц, плюс `String` в цикле = фрагментация кучи (§50).

---

## 5. Жизненный цикл модуля

```
        POST /api/v1/devices
                │
                ▼
      DeviceManager::validate()          ← чистая проверка по манифесту,
                │                          железо не трогается вообще
        ok ─────┴───── error ──► 400 + {code, field, detail}   (устройство не создано)
                │
                ▼
        factory() → IDevice*
                │
                ▼
      IDevice::configure(ctx)            ← claim() ресурсов
                │
        ok ─────┴───── error ──► releaseAllOwnedBy(self); delete; 409/422
                │
                ▼
           CONFIGURED
                │  begin()
                ▼
         INITIALIZING ──kTimeout──┐      ← повтор на следующем тике планировщика
                │                 │        (датчик может стартовать 100 мс)
                │◄────────────────┘
        ok ─────┴───── error ──► ERROR  (ресурсы удержаны, чтобы UI показал конфликт;
                │                        освобождаются при remove/reconfigure)
                ▼
            RUNNING ◄──────► WARNING     ← деградация: CRC-ошибки, редкие таймауты
                │
                │ ошибка связи N раз подряд
                ▼
             ERROR ──► DEVICE_ERROR event ──► каналы получают quality=FAULTED
                │
                │ DELETE /api/v1/devices/{key}
                ▼
        end(); releaseAllOwnedBy(); channels.removeAllFrom(); delete
                ▼
            DISABLED
```

Инварианты:

* из `ERROR` можно выйти только явной командой (`reconfigure`, `enable`) — прошивка
  не «переоткрывает» датчик молча, иначе оператор не узнает о проблеме;
* каналы удалённого устройства удаляются вместе с ним; дашборды, ссылающиеся
  на исчезнувший канал, показывают `MISSING CHANNEL`, а не падают;
* `SafetyManager` при переходе критического устройства в `ERROR` переводит связанные
  выходы в fail-safe **до** того, как пользовательские правила вообще увидят событие.

---

## 6. События Event Bus

| Событие | Кто публикует | Полезная нагрузка |
|---|---|---|
| `DEVICE_STATE_CHANGED` | DeviceManager | `source=device`, `integer=new state` |
| `DEVICE_ERROR` | DeviceManager / драйвер | `code`, `detail` |
| `DEVICE_CONNECTED` / `DEVICE_DISCONNECTED` | шинные менеджеры | `source=device` |
| `CHANNEL_UPDATED` | ChannelManager (**opt-in**) | `source=channel`, `number=value` |
| `EXPERIMENT_STARTED` / `_STEP_CHANGED` / `_STOPPED` | ExperimentManager | `integer=step` |
| `ALARM_TRIGGERED` / `ALARM_CLEARED` | RuleEngine | `code`, `severity` |
| `SAFETY_TRIPPED` | SafetyManager | `code`, `detail` |
| `CONFIG_CHANGED` | ConfigStorage | `integer=section` |
| `PROFILE_ACTIVATED` | ProfileManager | `detail=profile name` |
| `LOGGING_STARTED` / `LOGGING_STOPPED` | DataLogger | `detail=file` |
| `SYSTEM_MESSAGE` | любой | `severity`, `detail` |

**Важное ограничение, зафиксированное в M0.** EventBus — это шина *управления*, а не
данных. При 80 Гц на 20 каналов публикация каждого отсчёта через универсальную шину —
1600 диспетчеризаций в секунду впустую. Поэтому отсчёты идут через собственный
типизированный список слушателей `ChannelManager`, а `CHANNEL_UPDATED` включается
поканально и только там, где это действительно нужно (правила на медленных каналах,
`WAIT_UNTIL` в эксперименте). См. ADR-0002.

`Event` — POD 32 байта, без аллокаций; `detail` обязан указывать на строковый литерал
или интернированный символ.

---

## 7. Resource Manager

Единственный владелец дефицитного железа. Два вопроса, на которые он отвечает:

1. **Способность.** «GPIO34 может быть выходом?» — нет, у него нет выходного драйвера.
   «GPIO7?» — он припаян к flash. «GPIO4 как аналоговый вход?» — это ADC2, он не работает
   при включённом Wi-Fi. Источник истины — `ChipProfile`, таблица, а не `#ifdef`.
2. **Владение.** «GPIO21 свободен?» — нет, и ошибка содержит имя владельца:

```
RESOURCE_BUSY: used by I2C0 SDA
```

Отслеживаются: GPIO, ADC1/ADC2-каналы, шины I²C/SPI/UART, **адреса на конкретной шине
I²C** (0x38 на bus 0 и 0x38 на bus 1 — разные ресурсы), каналы и таймеры LEDC.

Ключевой метод — `releaseAllOwnedBy(DeviceHandle)`. Он делает откат частичного захвата
тривиально корректным: драйвер захватил DOUT, упал на SCK — менеджер откатывает всё
одной строкой. Классический баг «GPIO занят после неудачного добавления» здесь
структурно невозможен.

Валидация конфигурации происходит **до** создания драйвера, поэтому форма в браузере
может проверяться в реальном времени тем же кодом, который потом реально захватит пин.

---

## 8. Пример манифеста

Манифест — `static constexpr` во flash. Ниже — его JSON-проекция, которую отдаёт
`GET /api/v1/modules/sim_signal` и по которой SPA целиком строит форму.

```json
{
  "id": "sim_signal",
  "name": "Signal Simulator",
  "category": "sensor",
  "description": "Software signal source for developing dashboards, controllers and experiments without hardware",
  "bus": "none",
  "schema_version": 1,
  "max_instances": 0,
  "default_sample_interval_us": 100000,
  "min_sample_interval_us": 1000,
  "params": [
    { "key": "waveform", "label": "Waveform", "type": "select", "required": true,
      "default": "sine",
      "options": [
        { "value": "sine",        "label": "Sine" },
        { "value": "ramp",        "label": "Ramp (sawtooth)" },
        { "value": "square",      "label": "Square" },
        { "value": "triangle",    "label": "Triangle" },
        { "value": "constant",    "label": "Constant" },
        { "value": "random_walk", "label": "Random walk" }
      ] },
    { "key": "amplitude", "label": "Amplitude", "type": "float", "required": true,
      "default": 1.0, "min": -1e6, "max": 1e6, "step": 0.1 },
    { "key": "offset", "label": "Offset", "type": "float", "required": true,
      "default": 0.0, "min": -1e6, "max": 1e6, "step": 0.1 },
    { "key": "period_s", "label": "Period", "type": "float", "unit": "s",
      "required": true, "default": 10.0, "min": 0.05, "max": 3600,
      "visible_if": "waveform!=constant" },
    { "key": "noise", "label": "Noise amplitude", "type": "float",
      "required": false, "advanced": true, "default": 0.0, "min": 0 }
  ],
  "channels": [
    { "id": "value", "name": "Simulated value", "unit": "", "quantity": "raw",
      "direction": "input", "precision": 3 }
  ]
}
```

Так же выглядит и HX711 — с той разницей, что у него два параметра типа `gpio`
с `pin_use: "digital_input"` / `"digital_output"`, и SPA автоматически отрисует
селектор пинов, в котором недоступные пины уже погашены с причиной.

**Правило, без которого вся идея рассыпается:** если во frontend появляется
`if (module.id === 'hx711')` — значит, в `ParamSpec` не хватает поля. Расширяем
манифест, а не UI.

---

## 9. Формат основной конфигурации

Конфигурация разрезана на файлы по времени жизни и частоте записи — один общий
`config.json` означал бы перезапись всего при каждом сдвиге виджета и быстрый износ
flash (§47).

```
/config/
├── system.json        # hostname, часовой пояс, единицы, лог-уровень
├── devices.json       # экземпляры устройств + их конфиги + геометрия
├── channels.json      # переопределения имён, цветов, диапазонов, logged/visible
├── processing.json    # цепочки процессоров по каналам
├── calibrations.json  # калибровки + история версий
├── virtual.json       # формулы виртуальных каналов
├── control.json       # PID, термостаты, правила
├── experiments/*.json
├── dashboards/*.json
└── profiles/*.json    # снимок всего вышеперечисленного
```

`devices.json`:

```json
{
  "schemaVersion": 1,
  "devices": [
    {
      "key": "hx711_01",
      "module": "hx711",
      "name": "Sample balance",
      "enabled": true,
      "sample_interval_us": 12500,
      "config": { "data_pin": 16, "clock_pin": 17, "gain": 128 },
      "geometry": { "system": "cylindrical", "a": 0, "b": 0, "c": 0,
                    "group": "Balance", "role": "Sample mass" },
      "channels": {
        "mass": { "key": "mass_01", "name": "Sample mass", "unit": "g",
                  "precision": 3, "logged": true }
      }
    }
  ]
}
```

Правила формата:

* каждый файл начинается с `schemaVersion`; читатель, встретивший версию выше своей,
  возвращает `CONFIG_SCHEMA_TOO_NEW` и **не пытается угадать** (OTA-откат — штатная ситуация);
* запись атомарная: `*.tmp` → `fsync` → `rename`. Оборванная запись не уничтожает калибровку;
* ключи (`"hx711_01"`, `"mass_01"`) стабильны навсегда — на них ссылаются формулы,
  дашборды и старые CSV-логи;
* значения по умолчанию **не** сериализуются: их источник — манифест.

Подробности и миграции — `docs/storage.md`.

---

## 10. Соглашения REST API

Кратко; полностью — `docs/api.md`.

* Префикс `/api/v1`. Мажорная версия меняется только при несовместимом изменении.
* Ресурсы во множественном числе, идентификатор — стабильный ключ:
  `/api/v1/devices/hx711_01`.
* `GET` — чтение, `POST` — создание, `PATCH` — частичное изменение, `PUT` — полная замена,
  `DELETE` — удаление. `POST /…/{id}/actions/{name}` — команды (`tare`, `self-test`, `start`).
* Единый конверт ошибки, всегда одинаковой формы:

```json
{ "error": { "code": "GPIO_INPUT_ONLY", "numeric": 202,
             "message": "GPIO34 cannot be used as an output",
             "detail": "this pin has no output driver",
             "field": "clock_pin" } }
```

  `code` — стабильный символ из `core/Error.h`, на него переключается UI;
  `message` — для человека; `field` — чтобы подсветить конкретное поле формы.
* `POST /api/v1/devices?dry_run=1` выполняет только `validate()` — форма проверяется
  тем же кодом, что и реальное создание.
* Никаких «толстых» ответов: `GET /api/v1/channels` отдаёт дескрипторы, значения идут
  по WebSocket.
* Записывающие методы требуют аутентификации (M11); чтение — настраиваемо.

## 11. Протокол WebSocket

`/ws/live`, JSON (в M6 добавляется бинарный кадр для графиков). Полностью — `docs/websocket.md`.

Сервер → клиент, батч измерений:

```json
{ "type": "channels", "t": 1786550398241, "epoch": 1786550398241,
  "data":    { "12": 60.13, "13": 32.421, "14": 41.7 },
  "quality": { "14": "STALE" } }
```

* ключи — `ChannelHandle` в виде строки: короче ключей-имён и не меняются при
  переименовании канала пользователем;
* `quality` присылается **только при изменении** — в норме это пустой объект;
* батчинг по времени (`dashboard_rate`, по умолчанию 5 Гц), а не по отсчёту;
* если клиент не успевает (`ws->canSend()` = false), кадр **отбрасывается**, а не
  копится в очереди: телеметрия realtime, старый кадр никому не нужен, а очередь
  съест кучу.

Клиент → сервер: `subscribe` (подмножество каналов), `unsubscribe`, `ping`.
Управление (запись выхода, старт эксперимента) идёт по REST, а не по сокету —
чтобы у каждого действия были HTTP-код, аутентификация и запись в журнал.

---

## 12. Структура репозитория PlatformIO

```
lab-controller/
├── docs/                     # architecture, modules, channels, api, websocket,
│   └── adr/                  #   storage, experiments, frontend, hardware, development
├── firmware/
│   ├── platformio.ini        # env: esp32dev | esp32s3 | native
│   ├── partitions_4mb.csv    # 2×OTA + LittleFS 640 КБ + coredump
│   ├── partitions_8mb.csv
│   ├── src/
│   │   ├── main.cpp          # только композиция объектов и главный цикл
│   │   ├── core/             # без Arduino, собирается на хосте
│   │   ├── platform/{esp32,host}/
│   │   ├── buses/
│   │   ├── modules/{sensors,outputs,processing,control,virtual}/
│   │   │   └── BuiltinModules.cpp   # ЕДИНСТВЕННЫЙ список типов модулей
│   │   ├── services/         # DeviceManager, ChannelManager, ProcessingManager…
│   │   ├── storage/          # backend, ConfigStorage, ConfigApplier, JsonConfigView
│   │   ├── app/              # SystemManager, BootCounter
│   │   └── api/
│   ├── test/{test_core,test_services,test_devices,test_storage}/
│   └── data/www/             # собранный SPA (.gz), генерируется, в git не хранится
├── frontend/                 # Svelte 5 + TypeScript + Vite
└── tools/build_frontend.py   # pre-build hook: npm run build → gzip → data/www
```

---

## 13. Ключевые библиотеки

| Библиотека | Зачем | Обоснование выбора |
|---|---|---|
| **ArduinoJson 7** | конфигурация, REST, WS | де-факто стандарт, потоковая сериализация без промежуточной строки, предсказуемое потребление RAM |
| **PsychicHttp 2** | HTTP + WebSocket | поверх `esp_http_server` из ESP-IDF. Альтернатива `ESPAsyncWebServer` даёт более красивый API, но тянет собственный `AsyncTCP`-таск со своей историей падений и `Wcritical` при нехватке буферов. Приоритет проекта — надёжность (§49). Изолирован за `api/HttpServerAdapter`, замена стоит один файл |
| **LittleFS** (в ядре Arduino-ESP32) | frontend + конфиги | wear-leveling и устойчивость к обрыву питания, чего у SPIFFS нет |
| **uPlot** (frontend) | графики | ~45 КБ, тысячи точек без лагов на планшете. Chart.js/ECharts в 3–10 раз тяжелее и на мобильном ощутимо тормозят |
| **Unity** (via PlatformIO) | тесты | штатный runner `pio test` |

Не берём: динамические аллокаторы поверх кучи, шаблонные JSON-DSL, тяжёлые UI-киты,
всё, что тянет за собой `std::function` в горячем пути.

Драйверы датчиков пишем свои, а не берём Adafruit-семейство: почти все они блокирующие
(`delay()` внутри `read()`), что несовместимо с `poll()`-контрактом. Регистровые карты
простые, цена написания — десятки строк.

---

## 14. Риски архитектуры

| # | Риск | Последствие | Смягчение |
|---|---|---|---|
| R1 | **Фрагментация кучи** при частой переконфигурации | падение через часы работы | все контейнеры ядра фиксированного размера; `new` только в фазе «применить конфигурацию»; переконфигурация = полный снос и пересборка набора устройств, а не точечная правка; мониторинг `minFreeHeap` на странице диагностики |
| R2 | **Блокирующий драйвер** ломает всё | пропуски отсчётов, срабатывание WDT | контракт `poll()` неблокирующий; `Scheduler` меряет длительность каждой задачи и считает `overruns`; диагностика показывает нарушителя поимённо |
| R3 | **WebSocket топит CPU** при многих каналах и клиентах | лаги UI, потеря отсчётов | батчинг по времени; подписка на подмножество каналов; отбрасывание кадра при переполнении; лимит одновременных WS-клиентов |
| R4 | **Износ flash** от частых записей конфигурации | LittleFS умирает за месяцы | разделение на файлы; запись только по явному действию; debounce на перетаскивании виджетов; крупные логи — на SD |
| R5 | **ADC2 vs Wi-Fi** | молча неверные аналоговые измерения | `ResourceManager` запрещает ADC2-пины как аналоговый вход при включённом Wi-Fi — с явной ошибкой, а не «странными» числами |
| R6 | **Числовая неустойчивость калибровки** | полином на сырых счётчиках HX711 даёт мусор | `CalibrationSolver` всегда центрирует и масштабирует абсциссу, хранит `x_center`/`x_scale` вместе с коэффициентами, отдаёт RMS/max residual/R² в UI |
| R7 | **Циклы среди виртуальных каналов** | зависание/переполнение стека | граф формул проверяется на ацикличность при сохранении (`FORMULA_CYCLE`), пересчёт — в топологическом порядке |
| R8 | **Безопасность только через пользовательские правила** | перегрев установки | `SafetyManager` — отдельный слой с наивысшим приоритетом планировщика, игнорирует бюджет тика, не отключается из UI без явного подтверждения |
| R9 | **Разъезд манифеста и драйвера** | форма показывает поле, которого драйвер не читает | манифест — единственный источник; `validate()` бракует неизвестные ключи; в M1 добавляется тест «каждый ParamSpec.key читается драйвером» |
| R10 | **Рост SPA сверх раздела LittleFS** | не влезает вместе с конфигами | бюджет: JS+CSS ≤ 250 КБ gzip; `chunkSizeWarningLimit` в Vite; CI-проверка размера артефакта |
| R11 | **Потеря пользовательских данных при OTA** | потеря калибровок | конфиги в отдельном разделе LittleFS, который OTA не трогает; бэкап/экспорт перед обновлением; две OTA-слот-схемы с откатом |
| R12 | **Нет синхронного времени** | бесполезные метки в CSV | `IClock::epochMillis()` честно возвращает 0 до синхронизации; логгер пишет и монотонное, и настенное время и помечает несинхронизированные записи |

---

## 15. Критерии завершения Milestone 0

| # | Критерий | Статус |
|---|---|---|
| 1 | Архитектурная схема и модель сущностей зафиксированы | ✅ этот документ |
| 2 | Слои и правила зависимостей описаны; ядро не зависит от Arduino | ✅ собирается host-компилятором |
| 3 | Интерфейсы `IDevice`/`IOutputDevice`/`IProcessor`/`IController` объявлены | ✅ `core/IModule.h` |
| 4 | Жизненный цикл Device описан и отражён в `DeviceState` | ✅ |
| 5 | Формат манифеста определён, есть работающий пример | ✅ `sim_signal` |
| 6 | `ModuleRegistry` реализован, регистрация явная | ✅ + тест |
| 7 | `ResourceManager` реализован, валидирует и способность, и владение | ✅ + 4 теста |
| 8 | `Scheduler` реализован: приоритеты, бюджет, отсутствие burst-догона | ✅ + 4 теста |
| 9 | `EventBus` реализован, разделение control-plane / data-plane обосновано | ✅ + 3 теста, ADR-0002 |
| 10 | `ChannelManager` реализован: raw/calibrated/processed, pipeline, staleness | ✅ + 6 тестов |
| 11 | Формат конфигурации и стратегия версионирования/миграций описаны | ✅ `docs/storage.md` |
| 12 | Соглашения REST и протокол WebSocket описаны | ✅ `docs/api.md`, `docs/websocket.md` |
| 13 | Структура репозитория PlatformIO создана, `platformio.ini` для 3 окружений | ✅ |
| 14 | Список библиотек с обоснованием | ✅ §13 |
| 15 | Реестр рисков | ✅ §14 |
| 16 | Unit-тесты ядра проходят | ✅ 28 тестов, 0 падений |
| 17 | ADR по ключевым решениям | ✅ `docs/adr/0001…0008` |
| 18 | Есть программный датчик — разработка без железа возможна | ✅ `sim_signal` |

**Не входит в Milestone 0 (сознательно):** драйверы реального железа, REST/WS-реализация,
UI сверх каркаса, PID, правила, эксперименты, логгер, OTA, аутентификация.

---

## 16. Milestone 1 — Core ✅

### Что сделано

| # | Критерий | Статус |
|---|---|---|
| 1 | `DeviceManager`: валидация по манифесту с указанием проблемного поля | ✅ 7 тестов |
| 2 | Полный откат при любой ошибке: пины, каналы, задача планировщика, объект драйвера | ✅ тест «после неудачного `configure()` не остаётся ничего» |
| 3 | Повторяемый `begin()` (`kTimeout`) с ограничением числа попыток | ✅ тест |
| 4 | `ProcessingManager`: владение процессорами, сборка цепочки, авто-определение стадии калибровки | ✅ 5 тестов |
| 5 | Битая цепочка не ломает работающую (сборка в черновик, затем подмена) | ✅ тест |
| 6 | `ChannelManager`: хук жизненного цикла, чтобы pipeline не пережил свой канал | ✅ тест |
| 7 | `ConfigStorage`: `schemaVersion`, отказ читать файл из будущего, каркас миграций | ✅ 5 тестов |
| 8 | Атомарная запись; остаточный `.tmp` вычищается при загрузке | ✅ тест |
| 9 | `SystemManager`: порядок старта, safe mode по счётчику неудачных загрузок | ✅ 3 теста |
| 10 | Частичный отказ: одно битое устройство не мешает остальным подняться | ✅ тест |
| 11 | `main.cpp` — только композиция; ни одного имени устройства и ни одного GPIO | ✅ |
| 12 | **Критерий этапа:** устройство появляется и исчезает вместе со строкой в `devices.json` | ✅ `test_a_device_lives_and_dies_by_the_configuration_file` |

Итого: **61 тест, 0 падений**, сборка без предупреждений при `-Wall -Wextra -Wshadow`.

### Ключевые решения этапа

* **Документ — источник истины** (ADR-0010). `DeviceRecord` не хранит копию
  конфигурации драйвера; правка устройства — это read-modify-write над
  `devices.json` и повторное применение. Одна копия истины, экспорт = файлы с диска.
* **Один парсер на два входа.** `ConfigApplier::parseDeviceSpec` обслуживает и
  загрузку с диска, и (в M3) `POST /api/v1/devices`. Устройство, созданное через
  API, не может вести себя иначе, чем поднятое из файла.
* **Частичный отказ — норма.** `applyDevices()` возвращает отчёт, а не `Status`:
  один датчик с занятым пином не должен мешать одиннадцати остальным.
* **Safe mode.** Три неудачные загрузки подряд — и следующая проходит без
  запуска устройств. Конфигурацию, вешающую плату, можно починить по сети,
  а не паяльником.

## 17. Milestone 2 — First Hardware ✅

### Что сделано

| # | Критерий | Статус |
|---|---|---|
| 1 | Слой шин: `II2cBus`, `IGpioPort`, `IAdcPort`, `IBusProvider` — без Arduino | ✅ |
| 2 | `Esp32GpioPort` проверяет захват пина при каждом обращении | ✅ |
| 3 | `WireI2cBus` с таймаутом и повторным стартом при чтении регистра | ✅ |
| 4 | Сканер I²C с подсказками, помеченными как *possible* / *likely* | ✅ тест |
| 5 | AHT20: CRC-8, неблокирующая конверсия 80 мс, деградация WARNING→ERROR | ✅ 6 тестов |
| 6 | BMP280: проверка chip ID, целочисленная компенсация, отказ от значения сброса | ✅ 5 тестов |
| 7 | HX711: расширение знака, выбор усиления импульсами, ноль ожидания при обрыве | ✅ 5 тестов |
| 8 | `analog_in`, `digital_in` с антидребезгом и честностью про калибровку АЦП | ✅ 3 теста |
| 9 | Валидация: устройство на ненастроенной шине не создаётся, поле указано | ✅ тест |
| 10 | Один адрес на двух шинах — законно, на одной — конфликт с именем владельца | ✅ тест |
| 11 | **Критерий этапа:** вырванный провод даёт `DEVICE_NOT_RESPONDING` с внятным текстом | ✅ тесты AHT20 и HX711 |

Итого: **83 теста, 0 падений**, сборка без предупреждений при `-Wall -Wextra -Wshadow`.

### Ключевые решения этапа

* **Драйверы тестируются без железа** (ADR-0011). Все пять драйверов собираются
  хост-компилятором и проверяются на фальшивых шинах — включая ветки «датчика
  нет», «CRC не сошлась», «шина отвечает NACK», которые на живом стенде не
  воспроизвести.
* **Сверка двух реализаций вместо запомненных констант.** Целочисленная
  компенсация BMP280 проверяется против независимой реализации в `double` по
  всему диапазону АЦП. Две структурно разные формулы, сходящиеся в пределах
  0.02 °C и 2 Па, — это настоящее доказательство; пара «правильных» чисел из
  памяти — нет.
* **Подсказка остаётся подсказкой.** Сканер говорит «0x76: возможно BMP280 или
  BME280». Факт устанавливает только драйвер, прочитавший chip ID, — и если
  там оказался BME280, сообщение так и звучит.
* **HX711 не ждёт никогда.** Тест сравнивает счётчик потраченных микросекунд до
  и после трёх опросов при оторванном DOUT: ноль. Через три пропущенных периода
  конверсии — `DEVICE_NOT_RESPONDING` с текстом «check wiring and power».
* **DS18B20 отложен в M2.1 осознанно.** Битбэнг 1-Wire из кооперативной задачи
  ненадёжен: один только импульс сброса — это 480 мкс точного тайминга, и любое
  прерывание портит кадр. Правильное решение — периферия RMT, которая формирует
  и оцифровывает форму сигнала аппаратно. Это отдельная работа, а не ещё один
  драйвер.

## 18. Milestone 3 — API ✅

### Что сделано

| # | Критерий | Статус |
|---|---|---|
| 1 | `RestApi` не зависит от HTTP-библиотеки; адаптер только переводит | ✅ ADR-0012 |
| 2 | Роутер: сегменты, percent-decoding ключей, защита от длинного пути | ✅ 3 теста |
| 3 | Единый конверт ошибки `{code, numeric, message, detail, field}` | ✅ 2 теста |
| 4 | Честное отображение `ErrorCode` → HTTP (409 конфликт, 422 валидация, 507 нет места) | ✅ тест |
| 5 | `GET /modules` содержит всё для генерации формы: типы, диапазоны, опции, `pin_use` | ✅ тест |
| 6 | `GET /gpio` объясняет **почему** пин недоступен и кто им владеет | ✅ тест |
| 7 | `?dry_run=1` валидирует тем же кодом и не создаёт ничего | ✅ тест |
| 8 | Запись идёт через файл: сначала `devices.json`, потом запуск, при ошибке — откат | ✅ тест |
| 9 | Цепочка обработки применяется и сохраняется; неработающая не попадает в файл | ✅ тест |
| 10 | Экспорт/импорт конфигурации с полным восстановлением стенда | ✅ тест |
| 11 | Телеметрия: батчинг, подписка, quality только при изменении, drop вместо очереди | ✅ 5 тестов |
| 12 | **Критерий этапа:** датчик добавляется, настраивается и удаляется по HTTP | ✅ `test_a_sensor_is_added_configured_and_removed_over_http` |

Итого: **104 теста, 0 падений**, сборка без предупреждений при
`-Wall -Wextra -Wshadow`, весь набор чист под AddressSanitizer и UBSan.

### Ключевые решения этапа

* **API тестируется без сети** (ADR-0012). `handle(request, response)` — чистая
  функция; PsychicHttp появляется только в адаптере на ~200 строк.
* **Файл раньше объекта.** Создание устройства: записать в `devices.json` →
  поднять → при неудаче откатить файл. Устройство, которое работает, но не
  сохранено, исчезло бы при перезагрузке без объяснений.
* **`?dry_run=1` — тот же код.** Живая валидация формы вызывает ровно тот
  путь, который потом создаст устройство. Иначе она была бы второй догадкой.
* **Телеметрия отбрасывает кадр, но не данные.** При занятом сокете кадр
  теряется, а отметки «канал обновился» сохраняются — следующий кадр несёт
  более свежие значения. Ничего нужного не пропадает.

### Что нашёл AddressSanitizer

Прогон под ASan/UBSan вскрыл два дефекта, существовавших с Milestone 1 и не
проявлявшихся в обычных тестах:

1. **ArduinoJson хранит `const char*` по указателю.** Строка из временного
   `Error`, локального буфера `snprintf` или сегмента URL умирала до
   сериализации. Введён `jsonCopy()`; через него проходит всё, что не литерал.
2. **`deserializeJson(doc, char*, n)` парсит в zero-copy режиме.** Конфигурация
   читалась в буфер, который тут же освобождался, — каждый загруженный документ
   содержал висячие строки. Исправлено приведением к `const char*`.

Прогон под санитайзерами добавлен в обязательный чек-лист закрытия этапа.

## 19. Milestone 4 — Basic UI ✅

### Что сделано

| # | Критерий | Статус |
|---|---|---|
| 1 | Страницы Dashboard, Hardware, Channels, System, Diagnostics | ✅ |
| 2 | Мастер «Add Device» целиком из манифестов — ни одного `if (module.id === …)` | ✅ |
| 3 | Селектор пинов из `GET /gpio`: причина недоступности и владелец, без дублирования логики | ✅ |
| 4 | Живая валидация формы через `?dry_run=1` — тот же код, что и создание | ✅ |
| 5 | Сканирование I²C в мастере: адрес → кандидаты → форма | ✅ |
| 6 | Качество отсчёта видно всегда: stale / out of range / faulted не выдаются за норму | ✅ |
| 7 | Диагностика: статистика планировщика по задачам, куча, счётчики data plane | ✅ |
| 8 | Адаптивность: всё нужное у стенда доступно с телефона | ✅ скриншот 11 |
| 9 | Бюджет размера ≤ 250 КиБ gzip | ✅ **57 КиБ** |
| 10 | `svelte-check` без ошибок | ✅ 83 файла, 0 ошибок |
| 11 | **Критерий этапа:** полный цикл в браузере, без консоли и curl | ✅ прогон Playwright |

### Ключевое решение этапа

**Интерфейс разрабатывается против настоящей прошивки, а не против мока**
(ADR-0013). `tools/host_server` линкует реальные `RestApi`, `DeviceManager` и
драйверы и отдаёт их по сокету; `platform/host/HostBusProvider` изображает
плату, к которой ничего не подключено. Скриншоты в отчёте — снимки работающей
системы.

### Что это нашло

Три дефекта, которых не видел ни один из 104 unit-тестов:

1. **`?dry_run=1` расходился с созданием.** Валидация одобряла HX711 на сборке
   без GPIO-порта, создание падало с «incomplete device context». Расхождение
   между проверкой и действием подрывает саму идею живой валидации формы.
   Исправлено структурно: модуль **декларирует** потребность
   (`BusRequirement::kGpio` / `kAdc` / `kI2c`), `validate()` её проверяет.
2. **Драйвер, отказавший внутри `poll()`, оставался незамеченным.** Он выставлял
   себе `ERROR`, а `DeviceRecord` продолжал говорить `RUNNING`: API отдавал
   здоровое устройство, дашборд показывал последнее значение как свежее,
   `SafetyManager` (M8) тоже ничего бы не увидел. Добавлена сверка состояния
   после каждого опроса + тест.
3. **Карта GPIO не обновлялась.** Загружалась один раз при старте, поэтому после
   добавления устройства селектор предлагал уже занятые пины, а страница
   Hardware сообщала «пинов не занято».

Первые два — дефекты прошивки, а не интерфейса. Их нашёл именно прогон UI против
настоящего API.

## 20. Следующий этап — Milestone 5 (Calibration)

1. `CalibrationManager`: хранение калибровок с историей версий, привязка к
   каналам, `POST /calibrations/solve` уже описан в `docs/api.md`.
2. Редактор калибровки в UI: таблица эталонных точек, подгонка, показ невязок
   (RMS / max / R²) — калибровку, качество которой оператор не видит, применять
   нельзя.
3. Оставшиеся процессоры: median, low-pass, derivative, integral, statistics,
   clamp, deadband.
4. Редактор конвейера обработки: порядок стадий, включение/отключение.
5. Критерий готовности M5: тензодатчик калибруется по трём гирям в браузере,
   канал начинает показывать граммы, а стадии конвейера видны и переставляются.
