//! Demo host for the c2_display panel.
//!
//! Streams telemetry, notifications and sounds for a set number of seconds and
//! prints the PONG answers. Usage:
//!
//! ```text
//! cargo run --bin c2link-demo -- /dev/ttyACM0 30
//! cargo run --bin c2link-demo -- COM4 30
//! ```

use std::thread::sleep;
use std::time::{Duration, Instant};

use c2link::{
    channel, msg, sound, Config, Display, Notify, OdometerUnit, Pong, Severity, Telemetry,
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let path = args.next().unwrap_or_else(|| "/dev/ttyACM0".to_string());
    let seconds: u64 = args.next().and_then(|s| s.parse().ok()).unwrap_or(30);

    let mut dev = Display::open(&path)?;
    println!("connected to {path}, running for {seconds} s");

    // The panel boots with km; send the configuration once per connection.
    dev.config(&Config::default().odometer_unit(OdometerUnit::Km))?;

    let start = Instant::now();
    let mut next_telemetry = start;
    let mut next_event = start + Duration::from_secs(3);
    let mut next_ping = start + Duration::from_secs(10);
    let mut event = 0usize;

    while start.elapsed() < Duration::from_secs(seconds) {
        let now = Instant::now();
        let t = start.elapsed().as_secs_f32();

        if now >= next_telemetry {
            next_telemetry = now + Duration::from_secs(2);

            // Battery drains 95 % -> ~40 % over 30 s, odometer advances.
            let pct = (95.0 - t * 1.8).clamp(38.0, 100.0) as u8;
            let telemetry = Telemetry {
                voltage_cv: (4200 + pct as u32 * 17) as u16,
                batt_pct: Some(pct),
                flags: channel::CRSF | channel::WIFI | channel::VPN | channel::GPS_FIX,
                sats: 11 + (t as u8 % 4),
                odometer_dm: 6_341_200 + (t * 30.0) as u32,
            };
            dev.telemetry(&telemetry)?;
            println!(
                "tx telemetry: {:.2} V, {} %, flags 0x{:02X}, {} sats, {:.1} km",
                telemetry.voltage_cv as f32 / 100.0,
                pct,
                telemetry.flags,
                telemetry.sats,
                telemetry.odometer_dm as f32 / 10_000.0
            );
        }

        if now >= next_event {
            next_event = now + Duration::from_secs(9);
            let (notify, kind) = match event % 3 {
                0 => (
                    Notify::new("Starlink searching", Severity::Info).with_autohide(),
                    sound::SINGLE,
                ),
                1 => (
                    Notify::new("Battery low", Severity::Warning).with_blink(),
                    sound::DOUBLE,
                ),
                _ => (
                    Notify::new("C2 link lost", Severity::Error).with_blink(),
                    sound::TRIPLE,
                ),
            };
            dev.notify(&notify)?;
            dev.sound(kind)?;
            println!("tx notify: {:?} \"{}\"", notify.severity, notify.text);
            event += 1;
        }

        if now >= next_ping {
            next_ping = now + Duration::from_secs(10);
            dev.ping()?;
        }

        for frame in dev.poll()? {
            if frame.msg_type == msg::PONG {
                match Pong::decode(&frame.payload) {
                    Some(pong) => println!(
                        "pong: uptime {} ms, {} telemetry frames, echoed seq {}",
                        pong.uptime_ms, pong.rx_frames, pong.echoed_seq
                    ),
                    None => eprintln!("short pong ({} bytes)", frame.payload.len()),
                }
            } else {
                println!("rx frame type 0x{:02X} seq {}", frame.msg_type, frame.seq);
            }
        }

        sleep(Duration::from_millis(20));
    }

    dev.notify(&Notify::clear())?;
    println!("done");
    Ok(())
}
