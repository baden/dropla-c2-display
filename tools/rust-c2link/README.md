# c2link (Rust)

Клиент панели C2 по USB CDC: библиотека `c2link` + демо-бинарник `c2link-demo`.
Это эталонная реализация к руководству
[`docs/integration-rust.md`](../../docs/integration-rust.md).

```bash
cargo test                                  # CRC, «золотые» кадры, парсер
cargo run --bin c2link-demo -- /dev/ttyACM0 30
```

На Windows порт называется `COM4` (или другим номером):

```powershell
cargo run --bin c2link-demo -- COM4 30
```

Демо шлёт телеметрию каждые 2 с, уведомление и звук каждые 9 с, `PING` каждые
10 с и печатает `PONG`. Панель при этом показывает значения и сообщения.

## Что внутри

| Файл | Содержимое |
|---|---|
| `src/lib.rs` | CRC-16/CCITT-FALSE, сборка кадров, типы сообщений, побайтовый парсер, `Display` |
| `src/main.rs` | демо-поток телеметрии/уведомлений/звуков |

Зависимости: только [`serialport`](https://crates.io/crates/serialport) с
`default-features = false` (без libudev — открытие по пути работает на
минимальных Yocto-образах).

## Важно

- порт открывается один раз и держится открытым;
- DTR/RTS не трогать: на ESP32-S3 это команда сброса (`rst:0x15`);
  не вызывайте `write_data_terminal_ready()`/`write_request_to_send()` и не
  задавайте `.dtr_on_open(false)`;
- после перезагрузки панели `CONFIG` нужно отправить заново.

Сборка статического бинарника под aarch64 (i.MX95) — в
[`docs/integration-rust.md`](../../docs/integration-rust.md) §8.2.
