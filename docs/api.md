# REST API v1

**Статус: реализовано (Milestone 3).** Слой не зависит от HTTP-библиотеки:
`RestApi::handle(request, response)` — чистая функция, а адаптер PsychicHttp
только переводит одно в другое (ADR-0012). Поэтому маршрутизация, валидация,
коды ошибок и откат неудачного создания покрыты обычными unit-тестами.

Базовый префикс `/api/v1`. Все тела — JSON, UTF-8. Мажорная версия меняется только
при несовместимом изменении контракта.

## Соглашения

| Метод | Смысл |
|---|---|
| `GET /resources` | список дескрипторов (без значений измерений) |
| `GET /resources/{key}` | один объект |
| `POST /resources` | создание; `?dry_run=1` — только валидация |
| `PATCH /resources/{key}` | частичное изменение |
| `PUT /resources/{key}` | полная замена |
| `DELETE /resources/{key}` | удаление |
| `POST /resources/{key}/actions/{name}` | команда (`tare`, `self-test`, `start`, `stop`) |

* `{key}` — стабильный пользовательский ключ (`hx711_01`), не числовой handle.
  Handles живут только в WebSocket, где важна каждая пара байт.
* Списки не пагинируются: количество сущностей ограничено сверху лимитами прошивки.
* `GET` не изменяет состояние никогда, включая «безобидные» переинициализации.

## Формат ошибки

Один и тот же конверт для любого не-2xx ответа:

```json
{ "error": {
    "code": "I2C_ADDRESS_BUSY",
    "numeric": 206,
    "message": "I2C address 0x38 on bus 0 is already in use",
    "detail": "used by AHT20 #1",
    "field": "address" } }
```

`code` — стабильный символ из `core/Error.h`; именно на него переключается UI.
Формулировку `message` можно менять и переводить, `code` — нельзя.

| HTTP | Когда |
|---|---|
| 400 | синтаксис/типы запроса |
| 401 / 403 | нет аутентификации / недостаточно прав |
| 404 | нет такого ключа |
| 409 | конфликт ресурса (`RESOURCE_BUSY`, `ALREADY_EXISTS`) |
| 413 | `PAYLOAD_TOO_LARGE` |
| 422 | синтаксис верный, но конфигурация невалидна по манифесту |
| 429 | `RATE_LIMITED` |
| 500 | `INTERNAL` — обязательно попадает в системный лог |
| 507 | `FILESYSTEM_FULL` |

## Маршруты вне API

Веб-сервер на плате не использует `serveStatic`: маршруты перечислены явно и в
этом порядке (ADR-0021).

```
/health          GET   жив ли прибор — не трогает файловую систему вообще
/api/*           ANY   REST, целиком в RestApi
/ws/live         GET   телеметрия
/                GET   оболочка SPA
/index.html      GET   она же
/assets/*        GET   файл — или 404. НИКОГДА не оболочка
/*               GET   оболочка (клиентский маршрут)
```

`/health` отвечает `{"status":"ok","firmware":…,"schema_version":…,
"config_revision":…,"uptime_s":…}` и существует, чтобы на вопрос «плата
поднялась?» можно было ответить до того, как интерфейс залит, и независимо от
него. Пропавший ассет обязан быть 404: универсальный обработчик, отдающий на
`/assets/app.js` страницу `index.html`, превращает «интерфейс не залит» в
синтаксическую ошибку JavaScript в первой строке.

`tools/host_server` отвечает на те же URL так же. Это не удобство: Milestone 12
начался с того, что сервер разработки и плата маршрутизировали по-разному, и
поэтому отказ существовал только на железе.

## Дерево ресурсов

```
/api/v1/system                 GET  версия, чип, uptime, время, режим сети, отчёт о загрузке
/api/v1/system/reboot          POST
/api/v1/diagnostics            GET  heap, loop latency, задачи планировщика, ошибки шин

/api/v1/modules                GET  каталог типов (манифесты)
/api/v1/modules/{id}           GET  один манифест

/api/v1/buses                  GET  сконфигурированные шины
/api/v1/buses/i2c/{n}/scan     POST сканирование адресов
/api/v1/gpio                   GET  карта пинов: способности + владельцы

/api/v1/devices                GET|POST
/api/v1/devices/{key}          GET|PATCH|DELETE
/api/v1/devices/{key}/actions/{name}   POST

/api/v1/channels               GET|POST (виртуальные)
/api/v1/channels/{key}         GET|PATCH|DELETE
/api/v1/channels/{key}/value   GET  одиночное чтение (для скриптов, не для UI)
/api/v1/channels/{key}/write   POST запись в output-канал

/api/v1/processing/{channel}   GET|PUT цепочка процессоров
/api/v1/calibrations           GET|POST   ?channel=mass_01 — история версий
/api/v1/calibrations/solve     POST точки → коэффициенты (ничего не сохраняет)
/api/v1/calibrations/{id}      GET|DELETE (отказ, пока запись активна)
/api/v1/calibrations/{id}/activate    POST
/api/v1/calibrations/{id}/deactivate  POST

/api/v1/control                GET  блокировки, циклы и правила + живое состояние
/api/v1/control                PUT  заменить весь документ (?dry_run=1 — валидация)
/api/v1/control/loops/{id}/mode      POST {"mode":"off|manual|automatic"} — НЕ сохраняется
/api/v1/control/loops/{id}/setpoint  POST {"value":…} — сохраняется в control.json
/api/v1/control/loops/{id}/manual    POST {"value":…} — сохраняется
/api/v1/control/limits/{id}/reset    POST снять защёлку одной блокировки
/api/v1/control/limits/reset         POST снять все защёлки

/api/v1/dashboards             GET|POST
/api/v1/dashboards/{key}       GET|PUT|DELETE

/api/v1/experiments            GET  сводки сценариев + состояние запуска
/api/v1/experiments            POST создать (?dry_run=1 — проверить)
/api/v1/experiments/state      GET  текущий запуск: шаг, остаток, события
/api/v1/experiments/runs       GET  журнал запусков (метаданные §48)
/api/v1/experiments/{key}      GET|PUT|DELETE
/api/v1/experiments/{key}/actions/{start|pause|resume|stop}  POST
     start требует {"operator": …}; отказ при поднятом аварийном стопе

/api/v1/logs                   GET  индекс наборов + состояние записи и место
/api/v1/logs/start             POST ручная запись {channels, rate_hz, …}
/api/v1/logs/stop              POST
/api/v1/logs/{id}              GET|DELETE  (удаление — единственный способ
                                    освободить место; сам логгер не удаляет)
/api/v1/logs/{id}/export.csv   GET  потоковая отдача, не JSON-документ
/api/v1/system/log             GET  системный журнал (уровни DEBUG…CRITICAL)

/api/v1/outputs                GET  состояние всех выходов и аварийного стопа
/api/v1/outputs/trip           POST аварийный стоп: всё в безопасное состояние
/api/v1/outputs/clear          POST разрешить команды (ничего не включает)
/api/v1/outputs/{key}/renew    POST продлить срок команды, не меняя значение
/api/v1/outputs/{key}/release  POST отпустить выход в безопасное состояние

/api/v1/dashboards             GET|POST   GET — сводки, без раскладок
/api/v1/dashboards/{key}       GET|PUT|DELETE

/api/v1/profiles               GET|POST
/api/v1/profiles/{key}/activate POST
/api/v1/config/export          GET  полный снимок конфигурации
/api/v1/config/import          POST
/api/v1/firmware/ota           POST multipart, требует аутентификации
```

## Примеры

### Каталог модулей → форма

`GET /api/v1/modules` возвращает массив манифестов (см. `docs/architecture.md` §8).
SPA строит по нему и список «Add device», и саму форму. Отдельной страницы настроек
на каждый датчик не существует и не должно появиться.

### Карта пинов

`GET /api/v1/gpio` — то, чем питается селектор пинов:

```json
{ "chip": "esp32", "pins": [
  { "pin": 21, "usable": true,  "owner": "I2C0 SDA", "owner_device": "system" },
  { "pin": 34, "usable": true,  "input_only": true },
  { "pin": 7,  "usable": false, "reason": "GPIO_RESERVED", "note": "Connected to SPI flash" },
  { "pin": 12, "usable": true,  "advisory": "Strapping pin (MTDI): must be LOW at reset on 3.3 V flash" }
] }
```

Frontend не воспроизводит эту логику у себя — он её только отображает.

### Порядок записи: файл раньше объекта

Создание устройства — это три шага, и порядок в них принципиален:

```
1. записать запись в devices.json      ← сначала файл
2. поднять устройство                   
3. при неудаче — убрать запись из файла и сохранить снова
```

Устройство, которое работает, но не попало в файл, исчезнет при следующей
перезагрузке без всяких объяснений. Устройство, которое попало в файл, но не
поднялось, будет падать при каждой загрузке. Ни то, ни другое недопустимо,
поэтому конфигурация и живой стенд всегда согласованы (ADR-0010).

### Создание устройства с проверкой

```http
POST /api/v1/devices?dry_run=1
{ "key": "hx711_01", "module": "hx711", "name": "Sample balance",
  "sample_interval_us": 12500,
  "config": { "data_pin": 21, "clock_pin": 17, "gain": 128 } }
```

```json
{ "error": { "code": "RESOURCE_BUSY", "numeric": 200,
             "message": "GPIO21 is already in use",
             "detail": "used by I2C0 SDA", "field": "data_pin" } }
```

Тот же запрос без `dry_run` создаёт устройство и возвращает `201` с полным
`DeviceRecord`, включая созданные каналы.

`?dry_run=1` проходит **тот же** путь проверок, что и создание, включая
уникальность ключа: занятый ключ даёт `409 ALREADY_EXISTS` с `field: "key"` в
обоих случаях. Расхождение между проверкой и созданием — не мелочь: оно
превращает живую валидацию формы во вторую, отдельно написанную догадку.

### Кто это отвечает

`GET /api/v1/system` возвращает `controller_id` — устойчивое имя контроллера
(M14). Оно строится из eFuse MAC, не меняется при перепрошивке и не зависит от
сети. Адрес для этого не годится: в режиме точки доступа все платы отвечают на
`192.168.4.1`, а клиент, который хранит локальные записи по origin, обязан
уметь отличить один стенд от другого. Поле присутствует всегда, в том числе
когда сетевого блока в ответе нет.

### Что случилось при загрузке

`GET /api/v1/system` содержит блок `boot`. Устройство, которое записано в
`devices.json`, но не поднялось, не существует ни как запись, ни как канал — в
`/devices` его нет. Без этого блока оно исчезало молча, и «я его не добавлял»
выглядело ровно так же, как «оно упало, и никто не сказал» (§46).

```json
{ "boot": {
    "mode": "NORMAL", "storage_mounted": true,
    "buses_started": 1, "buses_failed": 0,
    "devices_started": 2, "devices_failed": 1,
    "processing_applied": 0, "processing_failed": 0,
    "first_failure": { "device": "sim_press", "field": "waveform",
                       "code": "DEVICE_CONFIG_INVALID", "numeric": 305,
                       "detail": "value is not one of the options" } } }
```

### Калибровка по эталонным точкам

```http
POST /api/v1/calibrations/solve
{ "channel": "mass_01", "kind": "linear", "unit": "g", "precision": 2,
  "points": [ { "raw": 453211, "reference": 0 },
              { "raw": 498322, "reference": 100 },
              { "raw": 543419, "reference": 200 } ] }
```

```json
{ "kind": "linear",
  "fit": { "coefficients": [100.0, 99.9999992], "order": 1,
           "x_center": 498317.33, "x_scale": 45104,
           "rms_residual": 0.00732, "max_residual": 0.01035,
           "r_squared": 0.999999992 },
  "residuals": [ { "raw": 453211, "reference": 0,
                   "predicted": -0.0052, "residual": 0.0052 }, … ],
  "stored": false }
```

`solve` **ничего не сохраняет** — превью, которое сохраняет, не превью. Тот же
запрос на `POST /api/v1/calibrations` создаёт версию `mass_01#1`, активирует её
(если не передано `"activate": false`) и возвращает `201`.

`residuals` отдаётся отдельно от сводки не для полноты: RMS 0.007 г прекрасно
скрывает одну гирю, промахнувшуюся на 0.4 г, и найти её можно только по точкам.

Коэффициенты бессмысленны без `x_center` / `x_scale` — солвер центрирует и
масштабирует абсциссу, иначе подгонка по отсчётам HX711 (~5·10⁵) строит систему
с элементами порядка 10²⁴ и решает её во float. Хранить и передавать их надо
вместе.

### Выходы

Запись в выход идёт через `POST /api/v1/channels/{key}/write` — **единственную**
дверь, и она проверяет аварийный стоп, состояние устройства и NaN (ADR-0016).
Ответ содержит и то, что попросили, и то, что получилось:

```json
{ "key": "heat", "unit": "%",
  "value": { "processed": 60.0, "quality": "GOOD" },
  "output": { "state": "COMMANDED", "safe_value": 0, "commanded": 100,
              "applied": 60, "hold_s": 120, "expires_in_s": 119.9 } }
```

`commanded` 100 при `applied` 60 — это работающий предел мощности нагревателя.
Блок `output` присутствует у каждого выходного канала везде, где канал
отдаётся: в списке, в одиночном запросе и в ответе на запись. Отдельного экрана
для актуаторов нет намеренно — нет и экрана, на котором актуатор выглядел бы
присмотренным.

`expires_in_s` считает прошивка, а не браузер: два источника времени разойдутся.

### Дашборды

`GET /api/v1/dashboards` отдаёт **сводки**: восемь дашбордов по двадцать четыре
виджета не помещаются в один ответ, а переключателю нужны имена, не раскладки.

```json
{ "dashboards": [ { "key": "overview", "name": "Overview", "widgets": 6,
                    "dangling_channels": 2 } ],
  "limits": { "dashboards": 8, "widgets": 24, "columns": 12 } }
```

`GET /api/v1/dashboards/{key}` — документ целиком плюс блок `health`:

```json
{ "health": { "widgets": 6, "dangling_channels": 2,
              "first_dangling": { "widget": "w3", "channel": "mass_01" } } }
```

Прошивка проверяет форму и не проверяет смысл (ADR-0015): `type` виджета для неё
непрозрачная строка, а `config` — непрозрачный объект. Что она обеспечивает:
уникальность ключей и `id`, целые координаты внутри 12 колонок, лимиты и размер
файла. Нарушение раскладки — `422 DASHBOARD_INVALID` с полем `widgets`;
переполнение — `OUT_OF_CAPACITY`.

Виджет, указывающий на исчезнувший канал, **не удаляется**: устройство могло не
подняться сегодня утром. Он назван в `health`, и интерфейс рисует плитку как
сломанную.

### Подбор калибровки

```http
POST /api/v1/calibrations/solve
{ "type": "linear",
  "points": [ { "raw": 453211, "reference": 0 },
              { "raw": 498322, "reference": 100 },
              { "raw": 543419, "reference": 200 } ] }
```

```json
{ "fit": { "order": 1, "coefficients": [100.0, 100.0],
           "x_center": 498317.33, "x_scale": 45104.0 },
  "quality": { "rms_residual": 0.0079, "max_residual": 0.0103, "r_squared": 0.9999999 } }
```

Метод ничего не сохраняет — оператор сначала смотрит на невязки, потом решает.

### Запись в исполнительное устройство

```http
POST /api/v1/channels/heater_power/write
{ "value": 40.0 }
```

Отклоняется с `SAFETY_INTERLOCK`, если активен интерлок; с `CHANNEL_TYPE_MISMATCH`,
если канал не является выходом. Управление идёт через REST, а не через WebSocket:
у каждой команды должны быть код ответа, аутентификация и след в журнале.

## Производительность и ограничения

* максимальный размер тела запроса — 12 КБ (`PAYLOAD_TOO_LARGE` сверх того);
  OTA идёт отдельным потоковым обработчиком (M11);
* ответ собирается в `JsonDocument` целиком, потолок — те же 12 КБ (ADR-0012).
  Экспорт конфигурации полного стенда ~8 КБ, запас двукратный. Эндпоинты, которые
  в принципе не помещаются в память (выгрузка CSV, M10), получат отдельный
  потоковый путь, а не увеличенный буфер;
* тяжёлые операции (`i2c/scan`, `config/export`) не блокируют цикл измерений:
  они выполняются в HTTP-таске и берут только короткие блокировки шины;
* конкурентные записи в конфигурацию сериализуются; при конфликте — `409`.

## Что реально отвечает прошивка (M3)

Реализованы: `/system`, `/system/reboot`, `/diagnostics`, `/modules`,
`/modules/{id}`, `/gpio`, `/buses`, `/buses/i2c/{n}/scan`, `/devices` (CRUD +
`?dry_run=1`), `/devices/{key}/actions/{self-test|enable|disable}`, `/channels`
(`?values=1`), `/channels/{key}`, `/channels/{key}/write`,
`/processing/{channelKey}` (GET/PUT), `/config/export`, `/config/import`.

Milestone 5 добавил `/calibrations`, Milestone 6 — `/dashboards`, Milestone 7 —
`/outputs`, Milestone 8 — `/control` (циклы, правила и блокировки в одном
документе: три списка, применяемые вместе, потому что применённые по отдельности
они на время расходятся).

Milestone 9 добавил `/experiments` — сценарии как **данные** (закрытый словарь
шагов, неизвестный `op` — ошибка), запуск с метаданными и журнал запусков.

Milestone 10 добавил `/logs`, потоковую отдачу CSV и включил шаги
`START_LOGGING` / `STOP_LOGGING`, которые до него намеренно отвергались.

### `/logs/{id}/export.csv`: второй путь ответа

`ApiResponse` строит документ в RAM и ограничен 12 КБ; набор данных — мегабайты.
Этот маршрут **описывает** файл (`StreamSpec`: путь, тип, имя), а транспорт его
передаёт: `PsychicFileResponse` прямо из LittleFS на плате, чанки в сокет на
хосте. REST-слой по-прежнему не знает про сокеты и тестируется целиком
(ADR-0012, ADR-0019).

Заполнение носителя останавливает **запись**, а не установку: набор данных
помечается усечённым в футере CSV, в индексе и в записи запуска, а эксперимент
продолжается (§49). Резерв в 64 КБ отсчёты не пересекают никогда.

### `/experiments`: что где живёт

Сценарий — конфигурация: `experiments.json`, переезжает вместе с экспортом.
Запись о запуске — **не** конфигурация: `/data/runs.json`, в экспорт не входит,
ограничена восемью последними. `state`, `reason` и `step_reached` в записи
пишутся вместе и никогда по отдельности — прерванный запуск не может читаться
как законченный (ADR-0018).

### `/control`: что операция, а что конфигурация

`PUT /control` — **конфигурация**: валидируется целиком (включая дубли `id`),
сохраняется, затем останавливает все циклы, освобождает удерживаемые выходы и
применяет заново. Менять коэффициенты под работающим регулятором нельзя.

`mode`, `setpoint`, `manual` и `limits/{id}/reset` — **операции**: отдельная
ручка на каждую, мгновенно, ничего больше не трогают. `mode` намеренно не
сохраняется: после перезагрузки любой цикл поднимается в `off` (ADR-0017).

`POST /outputs/clear` отказывает с `SAFETY_INTERLOCK`, пока защёлкнута хоть одна
блокировка: аварийный стоп не может служить способом выключить блокировку.

## Аутентификация (M11) ✅

`POST /api/v1/auth/login` → cookie сессии (`HttpOnly`, `SameSite=Strict`,
`Path=/`, 12 часов). Чтение открыто; запись требует входа. Реализовано в
ADR-0020.

```
/api/v1/auth                   GET  configured / signed_in / sessions / locked
/api/v1/auth/login             POST {"password": …}
/api/v1/auth/logout            POST
/api/v1/auth/password          POST {"current": …, "password": …}
                                    завершает ВСЕ сессии, включая текущую
/api/v1/firmware/ota           POST требует подтверждения паролем и отказывает
                                    при работающем запуске, идущей записи и
                                    любом скомандованном выходе
/api/v1/config/backup          GET  конфигурация, которую заменил последний импорт
```

**Освобождены от авторизации** (и это не оплошность, а §49): `POST
/outputs/trip`, `POST /outputs/{key}/release`, `POST
/experiments/{key}/actions/stop`. Человек, тянущийся к аварийному стопу, не
станет вводить пароль, а остановка не может сделать стенд менее безопасным.
**Снятие** стопа авторизации требует: это действие взведения.

**Подтверждения паролем поверх сессии** требуют действия, которые убирают
защиту: OTA, импорт конфигурации, смена пароля и `PUT /control`, если он удаляет
или отключает существующую блокировку. Сессия означает «этому браузеру когда-то
доверились», а не «человек хотел именно этого».

Хеш пароля лежит в `/config/auth.json`, который **не является** секцией
конфигурации, — поэтому экспорт не может его содержать даже по недосмотру.
