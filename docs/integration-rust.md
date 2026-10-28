# Интеграция с панелью C2: клиент на Rust

Практическое руководство: как реализовать обмен с панелью `c2_display` на Rust —
от открытия порта до готового модуля, который можно вставить в основной софт.
Спецификация протокола v1 — в [`link-protocol.md`](link-protocol.md), описание
экрана — в [`ui-spec.md`](ui-spec.md). Этот документ самодостаточен: всё
необходимое для реализации собрано здесь.

Эталонная реализация, по которой написан документ, лежит в
[`../tools/rust-c2link/`](../tools/rust-c2link/) — крейт `c2link` с библиотекой,
демо-бинарником и юнит-тестами на «золотые» кадры. Код из документа и код крейта
совпадают; крейт собран и прогнан на целевом устройстве (i.MX95, aarch64,
`/dev/ttyACM0`).

---

## Содержание

1. [Коротко: что нужно сделать](#1-коротко-что-нужно-сделать)
2. [Подключение и порт](#2-подключение-и-порт)
3. [Модель обмена](#3-модель-обмена)
4. [Формат кадра](#4-формат-кадра)
5. [Сообщения](#5-сообщения)
6. [Что при этом показывает панель](#6-что-при-этом-показывает-панель)
7. [Клиент на Rust](#7-клиент-на-rust)
8. [Сборка и платформы](#8-сборка-и-платформы)
9. [Тесты и проверка на железе](#9-тесты-и-проверка-на-железе)
10. [Диагностика](#10-диагностика)
11. [Чек-лист интеграции](#11-чек-лист-интеграции)
12. [Приложение: готовые кадры и константы](#12-приложение-готовые-кадры-и-константы)

---

## 1. Коротко: что нужно сделать

| Шаг | Действие |
|---|---|
| 1 | Открыть `/dev/ttyACM0` (Linux) один раз при старте, держать открытым |
| 2 | Не трогать DTR/RTS (см. §2.3) |
| 3 | Отправить `CONFIG` (единицы одометра), если нужны мили |
| 4 | Слать `TELEMETRY` каждые 1–3 с |
| 5 | По событиям — `NOTIFY` (текст в строку статуса) и `SOUND` |
| 6 | Раз в ~10 с — `PING`, читать `PONG` (диагностика и детект перезагрузки панели) |
| 7 | При ошибке записи — закрыть, переоткрыть, снова `CONFIG`, дальше по кругу |

Всё остальное — детали. Панель ничего не запрашивает: это односторонний поток
состояния с одним служебным ответом `PONG`.

---

## 2. Подключение и порт

### 2.1. Что за интерфейс

| Параметр | Значение |
|---|---|
| Интерфейс | USB CDC-ACM (нативный USB ESP32-S3, `USBMode=hwcdc`) |
| VID:PID | `303A:1001` (USB JTAG/serial debug unit) |
| Linux | `/dev/ttyACM0` (номер может быть другим) |
| Windows | `COMx`, «USB Serial Device» |
| Baud rate | 115200 — **номинальный**: USB CDC скорость не использует, значение нужно только библиотеке |
| Формат | 8N1, без flow control, без parity |
| Кабель | только data-кабель (не «charge only») |

### 2.2. Права доступа (Linux)

Пользователь должен быть в группе `dialout`:

```bash
sudo usermod -aG dialout "$USER"      # перелогиниться после этого
ls -l /dev/ttyACM0                    # crw-rw---- root dialout
```

Если панель должна быть доступна всегда под своим именем, можно добавить
udev-правило `/etc/udev/rules.d/99-c2-display.rules`:

```udev
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", SYMLINK+="c2display", MODE="0660", GROUP="dialout"
```

и открывать `/dev/c2display`.

### 2.3. DTR/RTS: самое важное

У ESP32-S3 линии DTR/RTS нативного USB — это **не** «звонки» модема, а вход
команды сброса USB-Serial/JTAG. Переключение линий в определённой
последовательности перезагружает чип (в логе видно
`rst:0x15 (USB_UART_CHIP_RESET)`).

Практика, проверенная на Windows и Linux:

| Платформа | Что делает открытие порта | Что должен делать клиент |
|---|---|---|
| Linux | tty-слой поднимает DTR/RTS при `open()`; S3 это **игнорирует** | **Ничего не менять** после `open()` |
| Linux | опускание DTR и RTS по одному (как делает pyserial) даёт промежуточную комбинацию = команда сброса | не вызывать `write_data_terminal_ready(false)`, `write_request_to_send(false)` |
| Windows | если выставить оба в 0 **до** `open()` (атомарно в DCB) — сброса нет | `serialport` по умолчанию линии не трогает; не вызывайте `.dtr_on_open(false)` |

В крейте `serialport` по умолчанию `dtr_on_open = None` — порт открывается без
вмешательства в линии, и панель не перезагружается (проверено: uptime между
запусками рос, см. §9.3). Явно задавать `.dtr_on_open(false)` **нельзя** — это
как раз опускание DTR после открытия.

Если панель всё же перезагрузилась (кто-то другой дёрнул линии, сработал
watchdog, пропадало питание):

- это не ошибка протокола — панель поднимется за ~2.7 с и покажет анимацию;
- кадры можно слать сразу: те, что пришли во время анимации, применятся после неё;
- `CONFIG` после перезагрузки теряется — отправьте его повторно (см. §7.2);
- `PONG.uptime_ms` начнётся с нуля — по нему удобно детектировать перезагрузку.

### 2.4. Порт открывается один раз

Не открывайте/закрывайте порт на каждое сообщение:

- на Linux каждое открытие — это raise линий (и риск сброса при неаккуратном
  обращении с DTR/RTS);
- на время открытия/закрытия панель может не принять кадры;
- в момент `open()` в буфере порта оказываются логи загрузки панели — их надо
  дочитать и выбросить (парсер это делает сам, см. §4.4).

---

## 3. Модель обмена

### 3.1. Направления

```
хост  ──TELEMETRY / NOTIFY / CONFIG / SOUND / PING──▶  панель
хост  ◀──────────────────── PONG ────────────────────  панель
```

- Панель **ничего не запрашивает** и не подтверждает кадры. Доставка не
  гарантируется: пропал кадр — пропало одно обновление, следующее всё исправит.
- Состояние идемпотентно: повторная отправка того же `TELEMETRY`/`CONFIG`
  безопасна. `SOUND` — команда, её повторять не нужно.
- Панель обрабатывает кадры по мере поступления; очередь команд не копится
  (звук — исключение: очередь на 4 сигнала, §5.4).

### 3.2. Ритм обмена

| Сообщение | Когда | Замечание |
|---|---|---|
| `TELEMETRY` | каждые 1–3 с | обязательный поток; панель считает данные устаревшими через 10 с |
| `PING` | раз в 10 с (или по необходимости) | ответ `PONG`; полезно для логов и детекта перезагрузки |
| `CONFIG` | при подключении и после перезагрузки панели | можно повторять раз в минуту — кадр крошечный |
| `NOTIFY` | по событию | держится на экране до замены/очистки (или 5 с с флагом автоскрытия) |
| `SOUND` | по событию | не чаще, чем панель успевает проигрывать (см. §5.4) |

Панель перерисовывает экран с частотой ~10 Гц, поэтому слать телеметрию чаще
5 раз в секунду смысла нет — лишняя нагрузка на USB и UART-буфер.

### 3.3. Sequence

`seq` — однобайтовый счётчик хоста, увеличивается на 1 с каждым кадром,
переполняется через 255. Панель его **не проверяет** и не дедуплицирует; она
только возвращает его в `PONG` (эхо), чтобы хост мог сопоставить ответ с
запросом. Если `seq` не нужен — всё равно инкрементируйте: это бесплатно и
сильно помогает в логах.

---

## 4. Формат кадра

### 4.1. Раскладка

```
 A5 5A | ver_type | seq | len_lo len_hi | payload ... | crc_lo crc_hi
 └─ magic ─┘   │       │    └─ длина, LE ─┘             └─ CRC16, LE ─┘
               │       └─ sequence (1..255, wraps)
               └─ (version << 4) | type
```

| Смещение | Размер | Поле |
|---|---|---|
| 0 | 2 | `A5 5A` — magic |
| 2 | 1 | `ver_type`: старший ниббл — версия (сейчас `1`), младший — тип |
| 3 | 1 | `seq` |
| 4 | 2 | длина payload, little endian (0…64) |
| 6 | N | payload |
| 6+N | 2 | CRC-16, little endian |

Итого кадр — `8 + N` байт. Максимум payload — **64 байта**; кадр с большей
длиной панель отбрасывает (и считает ошибку).

### 4.2. CRC

CRC-16/CCITT-FALSE: полином `0x1021`, начальное значение `0xFFFF`, без
отражения входа/выхода, без финального XOR. Считается по байтам
`ver_type … payload` (то есть по всему кадру, кроме magic и самого CRC).

Проверочное значение: `crc16("123456789") == 0x29B1`.

```rust
/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection).
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}
```

### 4.3. Сборка кадра

```rust
pub const MAGIC: [u8; 2] = [0xA5, 0x5A];
pub const VERSION: u8 = 1;
pub const MAX_PAYLOAD: usize = 64;

/// Обернуть payload в кадр. Паникует, если payload длиннее MAX_PAYLOAD.
pub fn encode(msg_type: u8, seq: u8, payload: &[u8]) -> Vec<u8> {
    assert!(payload.len() <= MAX_PAYLOAD, "payload too long");
    let mut out = Vec::with_capacity(8 + payload.len());
    out.extend_from_slice(&MAGIC);
    out.push((VERSION << 4) | (msg_type & 0x0F));
    out.push(seq);
    out.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    out.extend_from_slice(payload);
    let crc = crc16(&out[2..]);
    out.extend_from_slice(&crc.to_le_bytes());
    out
}
```

Кадры можно писать в порт подряд, без паузы: парсер панели потоковый, границы
кадров определяет по magic и длине.

### 4.4. Чтение: в потоке есть не только кадры

Важная особенность прошивки: **панель печатает свои логи в тот же USB CDC**
(`link: first telemetry: ...`, `link: notify ...`), а после сброса в потоке
появляется ещё и загрузочный лог ROM. Поэтому на входе хоста — смесь ASCII и
бинарных кадров.

Отсюда требования к приёмнику:

- искать magic `A5 5A` побайтово, а не «читать ровно 8 байт заголовка»;
- после ошибки CRC/длины — сбрасываться и продолжать сканировать с текущего
  байта (байт `A5` может быть и концом мусора, и началом кадра);
- всё, что не похоже на кадр, игнорировать (при желании — складывать в лог).

Готовый парсер (тот же алгоритм, что в прошивке) — в §7.3. Он проверен на
потоке, где кадры перемешаны с текстом.

---

## 5. Сообщения

| Тип | Имя | Payload | Направление |
|---|---|---|---|
| `0x1` | TELEMETRY | 9 байт | хост → панель |
| `0x2` | NOTIFY | 2 + N байт (N ≤ 63) | хост → панель |
| `0x3` | CONFIG | 1 + TLV | хост → панель |
| `0x4` | PING | пусто | хост → панель |
| `0x5` | SOUND | 2 + параметры | хост → панель |
| `0x8` | PONG | 10 байт | панель → хост |

Неизвестный тип панель игнорирует (пишет в лог и считает `rx_errors`), хост об
этом не уведомляет. Неизвестная версия протокола (не `1`) игнорируется молча —
это запас на будущий v2.

### 5.1. TELEMETRY (`0x1`) — 9 байт

| Смещение | Тип | Поле | Единицы |
|---|---|---|---|
| 0 | `u16 LE` | `voltage_cv` | санти-вольты: `5820` = 58.20 В |
| 2 | `u8` | `batt_pct` | 0…100 %, `255` = «неизвестно» |
| 3 | `u8` | `flags` | биты каналов, см. ниже |
| 4 | `u8` | `sats` | число спутников, 0…99 |
| 5 | `u32 LE` | `odometer_dm` | дециметры: `6_340_000` = 634.00 км |

Флаги `flags`:

| Бит | Маска | Плашка | Смысл |
|---|---|---|---|
| 0 | `0x01` | `CRSF` | радиолинк CRSF есть |
| 1 | `0x02` | `WIFI` | Wi-Fi подключён |
| 2 | `0x04` | `STARLINK` | Starlink онлайн |
| 3 | `0x08` | `LTE` | LTE онлайн |
| 4 | `0x10` | `VPN` | VPN поднят |
| 5 | `0x20` | `GPS` | есть фикс GPS |
| 6–7 | — | — | зарезервированы, шлите 0 |

Правила отображения:

- **Напряжение** показывается с одним знаком после точки, округление
  `(voltage_cv + 5) / 10`: `5824` → `58.2`, `5825` → `58.3`. Ноль и любые
  значения допустимы — это просто число.
- **Процент** — целое `batt_pct`. При `255` (и вообще > 100) процент скрыт,
  кольцо пустое — это «заряд неизвестен», а не «0 %».
- **Кольцо** — 36 сегментов, цвет сегмента фиксирован зоной:
  красный `0–20 %`, жёлтый `20–40 %`, зелёный `40–100 %`; заполненная часть
  яркая, остальная — тот же цвет, приглушённый.
- **Заряд < 10 %** (и данные свежие, и процент известен) — напряжение, `V` и
  процент мигают красным с периодом 1 с (500 мс видно / 500 мс нет).
- **Плашка GPS** горит, если выставлен бит `GPS_FIX` **или** `sats > 0`;
  число спутников пишется в плашке только при `sats > 0` (`GPS 14`).
- **Одометр** показывается целым числом: км — `(dm + 5000) / 10000`, мили —
  `(dm × 621371 + 5·10⁹) / 10¹⁰`, единица берётся из `CONFIG` (по умолчанию км).
  Дробной части на экране нет: `6_340_000` dm → `634 km`.
- **Нет данных**: пока не пришёл ни один кадр — `--.-` вместо напряжения,
  процент скрыт, все плашки серые, одометр `--`.
- **Устаревание**: если последний валидный `TELEMETRY` старше 10 с, значения
  тускнеют, плашки гаснут, в строке статуса появляется `Link lost` (янтарный).
  Данные при этом не стираются — вернулся поток, всё снова яркое.
- Кадр короче 9 байт отбрасывается (счётчик ошибок), длиннее — лишние байты
  игнорируются: дописывать поля в конец payload можно без смены версии.

### 5.2. NOTIFY (`0x2`) — текст в строку статуса

| Смещение | Тип | Поле |
|---|---|---|
| 0 | `u8` | `flags` |
| 1 | `u8` | `len` — длина текста в байтах |
| 2 | `len` | текст UTF-8 (без завершающего нуля) |

Биты `flags`:

| Бит | Маска | Смысл |
|---|---|---|
| 0–1 | `0x03` | важность: `0` debug, `1` info, `2` warning, `3` error |
| 2 | `0x04` | мигать, пока показывается (500 мс цвет / 500 мс фон) |
| 3 | `0x08` | автоскрытие через 5 с |
| 4–7 | — | зарезервированы, шлите 0 |

Цвета: debug `#5B6B7C`, info `#8695A5`, warning `#F59E0B`, error `#EF4444`.

Поведение:

- текст показывается сразу и заменяет предыдущее уведомление;
- **пустой текст очищает** строку статуса (так снимается «залипшее» сообщение);
- уведомление имеет приоритет над `Link lost`: пока оно на экране, строка
  показывает его, а не состояние линка;
- панель хранит **не больше 63 байт** текста (буфер 64 с нулём) — всё, что
  длиннее, обрезается; шлите не больше 80 байт (ограничение протокола);
- строка статуса — 32 px, ширина 300 px; длинный текст автоматически едет
  бегущей строкой;
- **шрифт встроенный, латиница (ASCII)**. Кириллица, эмодзи и прочий non-ASCII
  не отрисуются (в лучшем случае пустота). Пишите по-английски;
- обрезайте текст по границе UTF-8-символа, чтобы не разрезать многобайтовый
  символ (готовый `Notify::encode` в §7.3 это делает).

Пример: `notify "Starlink searching", severity=info, autohide` → в логе панели
`link: notify sev=1 flags=0x08 "Starlink searching"`.

### 5.3. CONFIG (`0x3`) — настройки отображения

```
count | tag len value | tag len value | ...
```

| Поле | Размер | Смысл |
|---|---|---|
| `count` | 1 | число TLV-элементов |
| `tag` | 1 | идентификатор настройки |
| `len` | 1 | длина значения |
| `value` | `len` | значение |

Известные теги:

| Тег | `len` | Значение | Смысл |
|---|---|---|---|
| `0x01` | 1 | `0` = км, `1` = мили | единицы одометра |
| `0x02` | 2 | `u16 LE`: `0`, `90`, `180`, `270` | поворот изображения |

Правила:

- неизвестные теги **игнорируются** — это точка расширения, новые настройки
  добавляются без смены версии протокола;
- при обрезанном/битом TLV панель прекращает разбор остатка кадра (остальное
  игнорируется), но уже применённые теги остаются;
- единицы одометра после перезагрузки сбрасываются в км, поэтому конфигурацию
  нужно отправить заново; **поворот** сохраняется в NVS и переживает перезагрузку;
- `count = 0` — валидный кадр-пустышка (ничего не меняет).

### 5.4. SOUND (`0x5`) — звуковые сигналы

| Смещение | Тип | Поле |
|---|---|---|
| 0 | `u8` | `kind` |
| 1 | `u8` | `flags` — зарезервировано, **0** |
| 2… | | параметры `kind` |

Готовые сигналы:

| `kind` | Имя | Что звучит |
|---|---|---|
| `0x00` | single | 1000 Гц, 120 мс |
| `0x01` | double | 1000 Гц 120 мс, пауза 90 мс, 1000 Гц 120 мс |
| `0x02` | triple | три раза 1000 Гц 120 мс, паузы 90 мс |
| `0x03` | long | 1000 Гц, 600 мс |
| `0x20` | charger connected | 800 Гц 120 мс, пауза 60 мс, 1200 Гц 160 мс |
| `0x21` | charging done | 1000/1300/1600 Гц по 140/140/180 мс, паузы 70 мс |

Произвольная последовательность (`kind = 0x10`):

| Смещение | Тип | Поле |
|---|---|---|
| 2 | `u8` | `count` — число тонов, 1…8 |
| 3 + 6·i | `u16 LE` ×3 | `freq_hz`, `on_ms`, `off_ms` для тона `i` |

- `freq_hz` — 100…8000 Гц, `on_ms` — 20…2000 мс, `off_ms` — 0…2000 мс;
  панель сама зажимает значения в эти диапазоны;
- `count = 0` — кадр игнорируется; тонов больше 8 — лишние отбрасываются,
  первые 8 играют;
- если кадр короче, чем `3 + count·6`, — ошибка, ничего не играет.

Очередь и приоритеты:

- очередь на **4 сигнала**, играются строго последовательно; при переполнении
  новый сигнал **отбрасывается** (в логе `sound kind=... dropped (queue full)`);
- сигнал не блокирует приём кадров и отрисовку — генерация идёт в отдельной
  задаче;
- `SOUND` — команда, а не состояние: после переподключения её повторять не надо;
- громкость — механическая крутилка на плате, программно не регулируется;
- при включении панель **молчит** (никаких тестовых писков).

### 5.5. PING (`0x4`) → PONG (`0x8`)

`PING` — пустой payload. Панель сразу отвечает `PONG` с payload 10 байт:

| Смещение | Тип | Поле | Смысл |
|---|---|---|---|
| 0 | `u8` | `version` | версия протокола (сейчас `1`) |
| 1 | `u32 LE` | `uptime_ms` | миллисекунды с момента загрузки панели |
| 5 | `u32 LE` | `rx_frames` | сколько валидных `TELEMETRY` принято с загрузки |
| 9 | `u8` | `echoed_seq` | `seq` того `PING`, на который это ответ |

Применение:

- проверка «панель жива и видит наш поток» без взгляда на экран;
- `uptime_ms` сбрасывается при перезагрузке → детект сброса/переподключения;
- `rx_frames` растёт, если телеметрия реально принимается (удобно для регрессий);
- `uptime_ms` переполняется через 49.7 суток — не используйте как абсолютное
  время, только как разницу.

`PONG` — единственное, что панель отправляет. Всё неизвестное на входе хоста
(логи, обрывки кадров) нужно просто игнорировать.

---

## 6. Что при этом показывает панель

### 6.1. Состояния экрана

| Условие | Экран |
|---|---|
| ни одного `TELEMETRY` ещё не было | `--.-` + `V`, процент скрыт, кольцо пустое, плашки серые, одометр `--` |
| идёт поток (кадр < 10 с назад) | значения яркие, плашки по флагам, кольцо по проценту |
| кадров нет > 10 с | значения тусклые, плашки гаснут, строка статуса: `Link lost` (янтарный) |
| есть уведомление | строка статуса показывает его (приоритет выше `Link lost`) |
| заряд < 10 % | напряжение/`V`/процент мигают красным, период 1 с |
| заряд `255` | процента нет, кольцо пустое (не «полное») |
| панель только что включилась | анимация ~2.7 с, затем реальные значения |

Время «свежести» — 10 с (`LINK_TIMEOUT_MS` в прошивке). Если телеметрия идёт
раз в 3 с, запас более чем трёхкратный; при периоде 5 с уже рискованно.

### 6.2. Анимация включения

| Момент | Что появляется |
|---|---|
| 0 мс | кольцо начинает заполняться 0 → 100 % |
| 1000 мс | кольцо заполнено, проявляются напряжение и `V` |
| 1250 мс | процент |
| 1450 мс | строка статуса |
| 1550 мс + 90 мс × i | плашки по одной (все зелёные) |
| 2300 мс | одометр с единицей |
| 2700 мс | всё «щёлкает» в реальные значения (или в прочерки, если данных нет) |

Кадры, пришедшие во время анимации, не теряются: они применяются в момент
2700 мс. Поэтому слать телеметрию можно сразу после открытия порта.

---

## 7. Клиент на Rust

### 7.1. Зависимости

```toml
[package]
name = "c2link"
version = "0.1.0"
edition = "2021"

[lib]
name = "c2link"
path = "src/lib.rs"

[[bin]]
name = "c2link-demo"
path = "src/main.rs"

[dependencies]
# libudev нужен только для serialport::available_ports(); открытие порта по
# имени работает и без него — так крейт собирается на минимальных Yocto-образах.
serialport = { version = "4.7", default-features = false }
```

Почему `serialport`:

- чистый Rust (кроме libc/nix), без C-зависимостей при `default-features = false`;
- одинаковый API на Linux/Windows/macOS;
- блокирующее чтение с таймаутом — для панели этого достаточно, async не нужен;
- `dtr_on_open` по умолчанию `None` — линии не трогаются (важно, см. §2.3).

Если нужен перечень портов (`available_ports()`), включите фичу `libudev` и
поставьте заголовки libudev (на Yocto — пакет `libudev`/`systemd-dev`). Если
основное ПО уже на tokio, вместо `serialport` можно взять `tokio-serial`
(обёртка над тем же `serialport`) — протокол от этого не меняется, меняется
только цикл чтения.

Альтернативы (`serial2`, `mio-serial`) не требуются; на i.MX95 проверен именно
`serialport 4.10`.

### 7.2. Архитектура

Рекомендуемая схема для основного софта:

```
 телеметрия (MAVLink/своя шина) ─┐
 события (каналы, заряд, GPS)  ──┼─▶ канал команд (mpsc) ─▶ поток панели ─▶ /dev/ttyACM0
 уведомления/звуки            ──┘        (последнее значение побеждает)
```

Правила, которые избавляют от большинства проблем:

1. **Один владелец порта.** `Display` живёт в отдельном потоке; остальной софт
   шлёт ему команды через `std::sync::mpsc`. Так медленный/зависший USB не
   тормозит полётный цикл, а запись никогда не перемешивается.
2. **Последнее значение побеждает.** Не копите очередь телеметрии: храните
   последний снимок и отправляйте его по таймеру (1–3 с). Если данных нет —
   просто не шлите, панель сама покажет `Link lost`.
3. **Переподключение.** При ошибке записи/чтения: закрыть порт, подождать
   0.5–2 с, открыть снова, отправить `CONFIG` и текущую телеметрию. Панель
   после сброса/переподключения начинает с прочерков, поэтому первый кадр после
   реконнекта должен быть полным снимком состояния.
4. **`CONFIG` — идемпотентно.** Отправляйте при подключении и после каждого
   реконнекта; при желании — раз в минуту (это 6–8 байт).
5. **`PING` — раз в 10 с.** Даёт `uptime`/`rx_frames` в логах и позволяет
   заметить перезагрузку панели (uptime уменьшился).
6. **Не блокируйтесь на записи.** USB CDC буферизует; при отключённом кабеле
   `write` вернёт ошибку — это сигнал к реконнекту, а не к повтору кадра.

### 7.3. Код: протокол

```rust
use std::io::{self, Read, Write};
use std::time::Duration;

pub const MAGIC: [u8; 2] = [0xA5, 0x5A];
pub const VERSION: u8 = 1;
pub const MAX_PAYLOAD: usize = 64;

pub mod msg {
    pub const TELEMETRY: u8 = 0x1;
    pub const NOTIFY: u8 = 0x2;
    pub const CONFIG: u8 = 0x3;
    pub const PING: u8 = 0x4;
    pub const SOUND: u8 = 0x5;
    pub const PONG: u8 = 0x8;
}

pub mod channel {
    pub const CRSF: u8 = 0x01;
    pub const WIFI: u8 = 0x02;
    pub const STARLINK: u8 = 0x04;
    pub const LTE: u8 = 0x08;
    pub const VPN: u8 = 0x10;
    pub const GPS_FIX: u8 = 0x20;
}

pub mod sound {
    pub const SINGLE: u8 = 0x00;
    pub const DOUBLE: u8 = 0x01;
    pub const TRIPLE: u8 = 0x02;
    pub const LONG: u8 = 0x03;
    pub const SEQUENCE: u8 = 0x10;
    pub const CHARGER_CONNECTED: u8 = 0x20;
    pub const CHARGING_DONE: u8 = 0x21;
}

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection).
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

pub fn encode(msg_type: u8, seq: u8, payload: &[u8]) -> Vec<u8> {
    assert!(payload.len() <= MAX_PAYLOAD, "payload too long");
    let mut out = Vec::with_capacity(8 + payload.len());
    out.extend_from_slice(&MAGIC);
    out.push((VERSION << 4) | (msg_type & 0x0F));
    out.push(seq);
    out.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    out.extend_from_slice(payload);
    let crc = crc16(&out[2..]);
    out.extend_from_slice(&crc.to_le_bytes());
    out
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Telemetry {
    pub voltage_cv: u16,
    pub batt_pct: Option<u8>,   // None => 255 на проводе
    pub flags: u8,
    pub sats: u8,
    pub odometer_dm: u32,
}

impl Telemetry {
    pub fn encode(&self) -> Vec<u8> {
        let mut p = Vec::with_capacity(9);
        p.extend_from_slice(&self.voltage_cv.to_le_bytes());
        p.push(self.batt_pct.unwrap_or(255));
        p.push(self.flags);
        p.push(self.sats);
        p.extend_from_slice(&self.odometer_dm.to_le_bytes());
        p
    }

    pub fn decode(p: &[u8]) -> Option<Self> {
        if p.len() < 9 {
            return None;
        }
        let pct = p[2];
        Some(Self {
            voltage_cv: u16::from_le_bytes([p[0], p[1]]),
            batt_pct: if pct <= 100 { Some(pct) } else { None },
            flags: p[3],
            sats: p[4],
            odometer_dm: u32::from_le_bytes([p[5], p[6], p[7], p[8]]),
        })
    }
}
```

Уведомления:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Severity { Debug = 0, Info = 1, Warning = 2, Error = 3 }

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Notify {
    pub severity: Severity,
    pub blink: bool,
    pub autohide: bool,
    pub text: String,
}

impl Notify {
    pub fn new(text: impl Into<String>, severity: Severity) -> Self {
        Self { severity, blink: false, autohide: false, text: text.into() }
    }

    /// Очистить строку статуса.
    pub fn clear() -> Self {
        Self::new(String::new(), Severity::Info)
    }

    pub fn with_blink(mut self) -> Self { self.blink = true; self }
    pub fn with_autohide(mut self) -> Self { self.autohide = true; self }

    pub fn encode(&self) -> Vec<u8> {
        let mut flags = self.severity as u8;
        if self.blink { flags |= 0x04; }
        if self.autohide { flags |= 0x08; }

        let raw = self.text.as_bytes();
        let mut len = raw.len().min(80);
        while len > 0 && !self.text.is_char_boundary(len) {
            len -= 1;
        }

        let mut p = Vec::with_capacity(2 + len);
        p.push(flags);
        p.push(len as u8);
        p.extend_from_slice(&raw[..len]);
        p
    }
}
```

Конфигурация и звук:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OdometerUnit { Km = 0, Miles = 1 }

/// Поворот изображения (тег `0x02`). Панель сохраняет его в NVS.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Rotation { Deg0, Deg90, Deg180, Deg270 }

impl Rotation {
    pub const fn degrees(self) -> u16 {
        match self {
            Rotation::Deg0 => 0,
            Rotation::Deg90 => 90,
            Rotation::Deg180 => 180,
            Rotation::Deg270 => 270,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Config {
    pub odometer_unit: Option<OdometerUnit>,
    pub rotation: Option<Rotation>,
}

impl Config {
    pub fn odometer_unit(mut self, unit: OdometerUnit) -> Self {
        self.odometer_unit = Some(unit);
        self
    }

    pub fn rotation(mut self, rotation: Rotation) -> Self {
        self.rotation = Some(rotation);
        self
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut items: Vec<(u8, Vec<u8>)> = Vec::new();
        if let Some(unit) = self.odometer_unit {
            items.push((0x01, vec![unit as u8]));
        }
        if let Some(rotation) = self.rotation {
            items.push((0x02, rotation.degrees().to_le_bytes().to_vec()));
        }
        let mut p = vec![items.len() as u8];
        for (tag, value) in items {
            p.push(tag);
            p.push(value.len() as u8);
            p.extend_from_slice(&value);
        }
        p
    }
}
```

Поворот применяется сразу и **сохраняется в NVS панели** (в отличие от единиц
одометра). Достаточно один раз отправить
`Config::default().rotation(Rotation::Deg90)` — при следующем включении панель
поднимется уже в этом положении. Допустимы `0`, `90`, `180`, `270`; другие
значения панель игнорирует.

Если у вас уже есть ведущее ПО, рассчитанное на предыдущую прошивку, порядок
перехода и подводные камни описаны в
[`host-migration-rotation.md`](host-migration-rotation.md).

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Tone {
    pub freq_hz: u16,   // 100..8000
    pub on_ms: u16,     // 20..2000
    pub off_ms: u16,    // 0..2000
}

pub fn sound_payload(kind: u8) -> Vec<u8> {
    vec![kind, 0]
}

pub fn sound_sequence_payload(tones: &[Tone]) -> Option<Vec<u8>> {
    if tones.is_empty() || tones.len() > 8 {
        return None;
    }
    let mut p = Vec::with_capacity(3 + tones.len() * 6);
    p.push(sound::SEQUENCE);
    p.push(0);
    p.push(tones.len() as u8);
    for t in tones {
        p.extend_from_slice(&t.freq_hz.to_le_bytes());
        p.extend_from_slice(&t.on_ms.to_le_bytes());
        p.extend_from_slice(&t.off_ms.to_le_bytes());
    }
    Some(p)
}
```

PONG:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Pong {
    pub version: u8,
    pub uptime_ms: u32,
    pub rx_frames: u32,
    pub echoed_seq: u8,
}

impl Pong {
    pub fn decode(p: &[u8]) -> Option<Self> {
        if p.len() < 10 {
            return None;
        }
        Some(Self {
            version: p[0],
            uptime_ms: u32::from_le_bytes([p[1], p[2], p[3], p[4]]),
            rx_frames: u32::from_le_bytes([p[5], p[6], p[7], p[8]]),
            echoed_seq: p[9],
        })
    }
}
```

Парсер (побайтовый автомат, терпит мусор и логи в потоке):

```rust
#[derive(Debug)]
pub struct Frame {
    pub version: u8,
    pub msg_type: u8,
    pub seq: u8,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State { Magic1, Magic2, Header, Payload, CrcLo, CrcHi }

#[derive(Debug)]
pub struct Parser {
    state: State,
    header: [u8; 4],
    header_len: usize,
    payload: Vec<u8>,
    payload_len: usize,
    crc_calc: u16,
    crc_rx: u16,
    errors: u32,
}

impl Parser {
    pub fn new() -> Self {
        Self {
            state: State::Magic1,
            header: [0; 4],
            header_len: 0,
            payload: Vec::with_capacity(MAX_PAYLOAD),
            payload_len: 0,
            crc_calc: 0xFFFF,
            crc_rx: 0,
            errors: 0,
        }
    }

    /// Сколько кадров отброшено (плохой CRC, слишком длинный payload).
    pub fn errors(&self) -> u32 { self.errors }

    pub fn feed(&mut self, byte: u8) -> Option<Frame> {
        match self.state {
            State::Magic1 => {
                if byte == MAGIC[0] { self.state = State::Magic2; }
                None
            }
            State::Magic2 => {
                if byte == MAGIC[1] {
                    self.state = State::Header;
                    self.header_len = 0;
                    self.crc_calc = 0xFFFF;
                } else if byte != MAGIC[0] {
                    self.state = State::Magic1;
                }
                None
            }
            State::Header => {
                self.header[self.header_len] = byte;
                self.header_len += 1;
                self.crc_calc = crc16_update(self.crc_calc, byte);
                if self.header_len == 4 {
                    self.payload_len =
                        u16::from_le_bytes([self.header[2], self.header[3]]) as usize;
                    self.payload.clear();
                    if self.payload_len > MAX_PAYLOAD {
                        self.errors += 1;
                        self.state = State::Magic1;
                    } else {
                        self.state = if self.payload_len == 0 {
                            State::CrcLo
                        } else {
                            State::Payload
                        };
                    }
                }
                None
            }
            State::Payload => {
                self.payload.push(byte);
                self.crc_calc = crc16_update(self.crc_calc, byte);
                if self.payload.len() >= self.payload_len {
                    self.state = State::CrcLo;
                }
                None
            }
            State::CrcLo => {
                self.crc_rx = byte as u16;
                self.state = State::CrcHi;
                None
            }
            State::CrcHi => {
                self.crc_rx |= (byte as u16) << 8;
                self.state = State::Magic1;
                if self.crc_rx == self.crc_calc {
                    let ver_type = self.header[0];
                    Some(Frame {
                        version: ver_type >> 4,
                        msg_type: ver_type & 0x0F,
                        seq: self.header[1],
                        payload: std::mem::take(&mut self.payload),
                    })
                } else {
                    self.errors += 1;
                    None
                }
            }
        }
    }
}

fn crc16_update(mut crc: u16, byte: u8) -> u16 {
    crc ^= (byte as u16) << 8;
    for _ in 0..8 {
        crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
    }
    crc
}
```

### 7.4. Код: обёртка над портом

```rust
pub struct Display {
    port: Box<dyn serialport::SerialPort>,
    seq: u8,
    parser: Parser,
}

impl Display {
    /// Открыть панель. Baud для USB CDC номинальный; DTR/RTS не трогаем.
    pub fn open(path: &str) -> serialport::Result<Self> {
        let port = serialport::new(path, 115_200)
            .timeout(Duration::from_millis(10))
            .open()?;
        Ok(Self { port, seq: 0, parser: Parser::new() })
    }

    pub fn send(&mut self, msg_type: u8, payload: &[u8]) -> io::Result<()> {
        if payload.len() > MAX_PAYLOAD {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "payload longer than 64 bytes",
            ));
        }
        self.seq = self.seq.wrapping_add(1);
        let frame = encode(msg_type, self.seq, payload);
        self.port.write_all(&frame)?;
        self.port.flush()
    }

    pub fn telemetry(&mut self, t: &Telemetry) -> io::Result<()> {
        self.send(msg::TELEMETRY, &t.encode())
    }

    pub fn notify(&mut self, n: &Notify) -> io::Result<()> {
        self.send(msg::NOTIFY, &n.encode())
    }

    pub fn config(&mut self, c: &Config) -> io::Result<()> {
        self.send(msg::CONFIG, &c.encode())
    }

    pub fn sound(&mut self, kind: u8) -> io::Result<()> {
        self.send(msg::SOUND, &sound_payload(kind))
    }

    pub fn sound_sequence(&mut self, tones: &[Tone]) -> io::Result<()> {
        let payload = sound_sequence_payload(tones).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "expected 1..=8 tones")
        })?;
        self.send(msg::SOUND, &payload)
    }

    pub fn ping(&mut self) -> io::Result<()> {
        self.send(msg::PING, &[])
    }

    /// Прочитать всё, что есть сейчас. Ждёт максимум таймаут порта (10 мс).
    pub fn poll(&mut self) -> io::Result<Vec<Frame>> {
        let mut buf = [0u8; 256];
        let mut frames = Vec::new();
        loop {
            match self.port.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => {
                    for &b in &buf[..n] {
                        if let Some(frame) = self.parser.feed(b) {
                            frames.push(frame);
                        }
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::TimedOut => break,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e),
            }
        }
        Ok(frames)
    }

    pub fn parse_errors(&self) -> u32 {
        self.parser.errors()
    }
}
```

`Display` — `Send`, поэтому его можно держать в отдельном потоке. Внутри —
только `serialport`-порт, счётчик `seq` и парсер; никаких блокировок.

### 7.5. Пример: поток телеметрии

```rust
use std::thread::sleep;
use std::time::{Duration, Instant};
use c2link::{channel, msg, Config, Display, Notify, OdometerUnit, Pong, Severity, Telemetry};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args().nth(1).unwrap_or_else(|| "/dev/ttyACM0".into());
    let mut dev = Display::open(&path)?;

    // Единицы одометра не переживают перезагрузку панели — шлём при подключении.
    // Поворот, наоборот, хранится в NVS; эта строка нужна только чтобы сменить его.
    dev.config(&Config::default().odometer_unit(OdometerUnit::Km))?;
    // dev.config(&Config::default().rotation(c2link::Rotation::Deg90))?;

    let start = Instant::now();
    let mut next_telemetry = start;
    let mut next_ping = start + Duration::from_secs(10);

    loop {
        let now = Instant::now();

        if now >= next_telemetry {
            next_telemetry = now + Duration::from_secs(2);
            dev.telemetry(&Telemetry {
                voltage_cv: 5820,                 // 58.20 В
                batt_pct: Some(87),
                flags: channel::CRSF | channel::WIFI | channel::GPS_FIX,
                sats: 14,
                odometer_dm: 6_341_200,           // 634.12 км -> на экране 634 km
            })?;
        }

        if now >= next_ping {
            next_ping = now + Duration::from_secs(10);
            dev.ping()?;
        }

        for frame in dev.poll()? {
            if frame.msg_type == msg::PONG {
                if let Some(pong) = Pong::decode(&frame.payload) {
                    println!(
                        "pong: uptime {} ms, {} telemetry frames, echoed seq {}",
                        pong.uptime_ms, pong.rx_frames, pong.echoed_seq
                    );
                }
            }
        }

        sleep(Duration::from_millis(20));
    }
}
```

Уведомление и звук по событию:

```rust
dev.notify(&Notify::new("Starlink searching", Severity::Info).with_autohide())?;
dev.sound(c2link::sound::SINGLE)?;

// тревога: мигающее сообщение + двойной сигнал
dev.notify(&Notify::new("Battery low", Severity::Warning).with_blink())?;
dev.sound(c2link::sound::DOUBLE)?;

// событие закончилось — убрать сообщение
dev.notify(&Notify::clear())?;
```

### 7.6. Готовый крейт

`tools/rust-c2link/` — тот же код плюс:

- `Pong::encode`, `Config::decode`, `Notify::decode`, `Telemetry::decode` —
  для тестов и для приёма кадров (пригодится, если панель когда-нибудь начнёт
  что-то кроме `PONG`);
- 13 юнит-тестов на CRC, сборку всех типов кадров, парсер (ресурс после мусора,
  битый CRC, слишком длинный payload), границу UTF-8 в `Notify`;
- демо `c2link-demo`: телеметрия каждые 2 с, уведомление + звук каждые 9 с,
  `PING` каждые 10 с, печать `PONG`.

```bash
cd tools/rust-c2link
cargo test
cargo run --bin c2link-demo -- /dev/ttyACM0 30
```

---

## 8. Сборка и платформы

### 8.1. Разработка (x86_64 Linux/macOS/Windows)

```bash
cargo test
cargo build --release
cargo run --bin c2link-demo -- /dev/ttyACM0
```

На Windows порт называется `COM4` (или другим номером):

```powershell
cargo run --bin c2link-demo -- COM4 30
```

### 8.2. Целевое устройство (aarch64, i.MX95)

Самый простой путь — статический бинарник под musl, без sysroot и без
зависимости от библиотек образа:

```bash
rustup target add aarch64-unknown-linux-musl
```

`.cargo/config.toml` в проекте (или `~/.cargo/config.toml`):

```toml
[target.aarch64-unknown-linux-musl]
linker = "rust-lld"
```

```bash
cargo build --release --target aarch64-unknown-linux-musl
scp target/aarch64-unknown-linux-musl/release/c2link-demo root@<ip>:/tmp/
```

На устройстве:

```bash
chmod +x /tmp/c2link-demo
/tmp/c2link-demo /dev/ttyACM0 15
```

Проверено на `imx95-15x15-lpddr4x-frdm` (Yocto, aarch64): бинарник статический,
`file` показывает `ELF 64-bit LSB executable, ARM aarch64, statically linked`;
запуск без танцев с библиотеками.

Если нужен glibc-вариант (например, для отладки с `perf`), собирайте
`aarch64-unknown-linux-gnu` с sysroot из Yocto SDK — но для приложения это не
требуется.

### 8.3. Yocto / минимальный образ

- `default-features = false` у `serialport` убирает зависимость от libudev:
  открытие по пути работает, `available_ports()` — нет;
- libc/nix — единственные системные зависимости, они есть в любом образе;
- если нужен поиск порта по VID:PID без libudev, читайте
  `/sys/class/tty/*/device/../idVendor` или используйте udev-симлинк (§2.2);
- для автозапуска удобен systemd-юнит с `After=dev-ttyACM0.device` и
  `Restart=always`: реконнект-логика всё равно нужна, но юнит подстрахует.

### 8.4. Права и udev

См. §2.2. Если софт работает под отдельным пользователем — добавьте его в
`dialout` или сделайте udev-правило с `GROUP=` и `MODE="0660"`.

---

## 9. Тесты и проверка на железе

### 9.1. Юнит-тесты (без панели)

В крейте зафиксированы «золотые» кадры — те же, что в
[`link-protocol.md`](link-protocol.md) §6. Они ловят ошибки в CRC, порядке
байтов и раскладке полей:

| Что | Кадр (hex) |
|---|---|
| telemetry `58.20 В, 95 %, 0x3F, 14, 634 km`, seq 1 | `A5 5A 11 01 09 00 BC 16 5F 3F 0E A0 BD 60 00 1D 59` |
| notify «очистить», seq 3 | `A5 5A 12 03 02 00 01 00 5F AF` |
| config `mi`, seq 4 | `A5 5A 13 04 04 00 01 01 01 01 0C 61` |
| ping, seq 5 | `A5 5A 14 05 00 00 66 BE` |
| sound double, seq 2 | `A5 5A 15 02 02 00 01 00 4F CD` |
| sound sequence `800:120:60, 1200:160:0`, seq 7 | `A5 5A 15 07 0F 00 10 00 02 20 03 78 00 3C 00 B0 04 A0 00 00 00 3C 98` |

Плюс проверки парсера: мусор перед кадром, битый CRC (кадр отбрасывается,
следующий находится), payload > 64 (кадр отбрасывается), граница UTF-8.

```bash
cargo test
```

### 9.2. Проверка через Python-клиент

`tools/c2link.py` — независимая реализация протокола; удобно перекрёстно
проверять поведение:

```bash
python3 tools/c2link.py --port /dev/ttyACM0 telemetry --voltage 58.2 --pct 95 --odo 634
python3 tools/c2link.py --port /dev/ttyACM0 notify "Starlink searching" --severity info --hide
python3 tools/c2link.py --port /dev/ttyACM0 sound double
python3 tools/c2link.py --port /dev/ttyACM0 ping
```

В ответ панель печатает свои логи (`link: first telemetry: ...`) — это
подтверждение, что кадр разобран.

### 9.3. Проверка Rust-крейта на панели

Реальный прогон на i.MX95 (статический бинарник из §8.2):

```
connected to /dev/ttyACM0, running for 15 s
tx telemetry: 58.15 V, 95 %, flags 0x33, 11 sats, 634.1 km
...
pong: uptime 1413749 ms, 15 telemetry frames, echoed seq 9
```

Что это подтверждает:

- 15 отправленных кадров = 15 принятых панелью (`rx_frames`) — весь конвейер
  работает, включая CRC;
- `uptime` ~23.6 минуты — панель **не перезагрузилась** при открытии порта
  крейтом `serialport` (сравните с `rst:0x15` при неаккуратном DTR/RTS);
- `echoed seq 9` — ответ привязан к отправленному `PING`.

Визуально на экране: напряжение/процент/плашки/одометр из демо, затем
`Starlink searching`, `Battery low` (мигает) и звуковые сигналы.

---

## 10. Диагностика

| Симптом | Причина / что делать |
|---|---|
| `No such file or directory` | панель не подключена/нет `cdc_acm`; проверьте `dmesg`, `ls /dev/ttyACM*` |
| `Permission denied` | пользователь не в `dialout`; udev-правило (§2.2) |
| Панель перезагружается при открытии порта | кто-то дёргает DTR/RTS: уберите `write_data_terminal_ready`/`write_request_to_send`, не ставьте `.dtr_on_open(false)`. На Linux tty сам поднимает линии — это нормально и сброса не даёт |
| Кадры уходят, экран не меняется | не тот порт (например, UART-переходник); проверьте `PING` → `PONG` и `rx_frames` |
| `PONG` не приходит | панель занята загрузкой (~2.7 с) или кадр `PING` битый; проверьте CRC и порядок байтов |
| Через 10 с появляется `Link lost` | телеметрия не идёт или идёт реже 1/10 с; проверьте таймер отправки |
| Процент не показывается | `batt_pct = 255` (unknown) или > 100 |
| Одометр показывает не то | единицы: `odometer_dm` в дециметрах (×10 000 на км), на экране целые км/мили по `CONFIG` |
| Уведомление не появляется | пустой текст (`len = 0`) очищает строку; кириллица не рендерится (только ASCII) |
| Уведомление висит | нет флага автоскрытия и не пришло новое/пустое — отправьте `Notify::clear()` |
| Звука нет | механическая громкость на плате; проверьте лог `link: sound kind=...`; при переполнении очереди — `dropped (queue full)` |
| Звук «залип»/не тот | `kind` из таблицы §5.4; неизвестный `kind` игнорируется с логом |
| В логе `link: crc error` | ошибка в CRC или порядке байтов; сверьтесь с «золотыми» кадрами §9.1 |
| Мусор в логах хоста | это логи панели в том же CDC; парсер их игнорирует, не читайте поток «по 8 байт» |

---

## 11. Чек-лист интеграции

- [ ] порт открыт один раз, DTR/RTS не трогаются;
- [ ] `PING` → `PONG` приходит, `rx_frames` растёт;
- [ ] `CONFIG` отправлен при старте и после реконнекта;
- [ ] телеметрия идёт каждые 1–3 с, `Link lost` не появляется;
- [ ] все шесть плашек проверены (по одной и вместе), GPS с числом спутников;
- [ ] заряд `255` — процента нет, кольцо пустое;
- [ ] заряд < 10 % — мигание красным;
- [ ] уведомления: info/warning/error, blink, autohide, очистка пустой строкой;
- [ ] звуки: single/double/triple/long/charge-start/charge-done + своя
      последовательность;
- [ ] переподключение: выдернуть кабель, вставить, дождаться восстановления
      (конфиг + полный снимок телеметрии);
- [ ] панель после передёргивания питания показывает анимацию и затем данные;
- [ ] в логах нет `crc error` и `payload longer than 64`.

---

## 12. Приложение: готовые кадры и константы

### 12.1. Константы

| Константа | Значение | Где используется |
|---|---|---|
| magic | `A5 5A` | начало кадра |
| версия протокола | `1` | старший ниббл `ver_type` |
| max payload | 64 байта | больше — кадр отбрасывается |
| таймаут «свежести» | 10 000 мс | после — `Link lost`, значения тускнеют |
| автоскрытие уведомления | 5 000 мс | флаг `0x08` |
| период мигания | 500 мс | уведомления и низкий заряд |
| порог низкого заряда | < 10 % | мигание красным |
| сегментов кольца | 36 | зоны 0–20 / 20–40 / 40–100 % |
| тонов в последовательности | 1…8 | `SOUND` kind `0x10` |
| очередь звуков | 4 | переполнение — сигнал отбрасывается |
| частота тонов | 100…8000 Гц | зажимается прошивкой |
| длина тона | 20…2000 мс | зажимается прошивкой |
| буфер текста уведомления | 63 байта | лишнее обрезается |
| анимация включения | 2 700 мс | до этого экран показывает анимацию |

### 12.2. Готовые кадры

Все кадры — с конкретным `seq`; при отправке подставьте свой (это меняет CRC).

```
telemetry:  58.20 V, 95 %, flags 0x3F, 14 sats, 634.00 km
  A5 5A 11 01 09 00 BC 16 5F 3F 0E A0 BD 60 00 1D 59

notify:     очистить строку, seq 3
  A5 5A 12 03 02 00 01 00 5F AF

notify:     "Starlink searching", info, autohide, seq 2
  A5 5A 12 02 14 00 09 14 53 74 61 72 6C 69 6E 6B
  20 73 65 61 72 63 68 69 6E 67 6D 8C

config:     одометр в милях, seq 4
  A5 5A 13 04 04 00 01 01 01 01 0C 61

ping:       seq 5
  A5 5A 14 05 00 00 66 BE

pong:       uptime 38731 мс, 0 кадров, эхо seq 1 (seq PONG = 1)
  A5 5A 18 01 0A 00 01 4B 97 00 00 00 00 00 00 01 FC FF

sound:      double, seq 2
  A5 5A 15 02 02 00 01 00 4F CD

sound:      последовательность 800 Гц/120/60, 1200 Гц/160/0, seq 7
  A5 5A 15 07 0F 00 10 00 02 20 03 78 00 3C 00 B0 04
  A0 00 00 00 3C 98
```

### 12.3. Минимальный «hello» для проверки

```rust
let mut dev = Display::open("/dev/ttyACM0")?;
dev.config(&Config::default().odometer_unit(OdometerUnit::Km))?;
dev.telemetry(&Telemetry {
    voltage_cv: 5820,
    batt_pct: Some(95),
    flags: channel::CRSF | channel::WIFI | channel::GPS_FIX,
    sats: 14,
    odometer_dm: 6_340_000,
})?;
dev.ping()?;
```

На экране: `58.2` + `V`, `95%`, кольцо на 95 %, зелёные `CRSF`/`WIFI`/`GPS 14`,
одометр `634 km`; в ответ — `PONG`.
