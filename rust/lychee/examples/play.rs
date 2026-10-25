//! Plays a file or URL headless, pulling a frame every 16 ms as a 60 Hz
//! display would, and prints events and the final stats.
//!
//!     cargo run --example play -- /tmp/lychee-media/h264_aac.mp4 [frame.ppm]
//!
//! With a second argument it writes the frame shown two seconds in as a
//! PPM image.

use std::sync::mpsc;
use std::time::{Duration, Instant};

use lychee::{Config, FrameType, Player, State};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let url = args.next().ok_or("usage: play URL [FRAME.ppm]")?;
    let snapshot = args.next();

    let player = Player::new(&Config::default())?;
    let (events, received) = mpsc::channel();
    player.on_event(move |event| {
        let _ = events.send(event);
    });
    player.open(&url)?;
    player.play();

    let start = Instant::now();
    let mut frames = 0;
    let mut wrote_snapshot = snapshot.is_none();
    loop {
        for event in received.try_iter() {
            println!("{:>6} ms  {event:?}", start.elapsed().as_millis());
        }
        match player.state() {
            State::Ended | State::Error => break,
            _ if start.elapsed() > Duration::from_secs(60) => break,
            _ => {}
        }
        let display_time = lychee::now_us() + 16_667;
        if player.has_new_frame(display_time)
            && let Some(frame) = player.acquire_frame(display_time)
        {
            frames += 1;
            if !wrote_snapshot && frame.pts_us() >= 2_000_000 {
                wrote_snapshot = true;
                let path = snapshot.as_deref().unwrap();
                write_ppm(&frame, path)?;
                println!("wrote {path} ({:?})", frame);
            }
        }
        std::thread::sleep(Duration::from_millis(16));
    }

    println!("state {:?}, error {:?}", player.state(), player.error());
    println!("{} frames pulled, {:?}", frames, player.stats());
    Ok(())
}

fn write_ppm(frame: &lychee::Frame, path: &str) -> std::io::Result<()> {
    let (data, stride) = frame.data().ok_or(std::io::ErrorKind::Unsupported)?;
    let (width, height) = (frame.width() as usize, frame.height() as usize);
    let (r, b) = match frame.kind() {
        Some(FrameType::CpuRgba) => (0, 2),
        _ => (2, 0),
    };
    let mut out = format!("P6\n{width} {height}\n255\n").into_bytes();
    for row in data.chunks(stride).take(height) {
        for pixel in row[..width * 4].chunks(4) {
            out.extend_from_slice(&[pixel[r], pixel[1], pixel[b]]);
        }
    }
    std::fs::write(path, out)
}
