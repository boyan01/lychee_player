//! Plays a file or URL in a desktop window using CPU frames.
//!
//!     cargo run --example window -- [--seconds N] URL

use std::time::{Duration, Instant};

use lychee::{Config, Frame, Player, State};
use minifb::{Key, Window, WindowOptions};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let first = args.next().ok_or("usage: window [--seconds N] URL")?;
    let (limit, url) = if first == "--seconds" {
        let seconds: f64 = args.next().ok_or("missing seconds")?.parse()?;
        if !seconds.is_finite() || seconds <= 0.0 {
            return Err("seconds must be positive and finite".into());
        }
        (
            Some(Duration::try_from_secs_f64(seconds)?),
            args.next().ok_or("missing URL")?,
        )
    } else {
        (None, first)
    };
    if args.next().is_some() {
        return Err("usage: window [--seconds N] URL".into());
    }

    let player = Player::new(&Config::default())?;
    let mut window = Window::new(
        "Lychee",
        800,
        450,
        WindowOptions {
            resize: true,
            ..WindowOptions::default()
        },
    )?;
    window.set_target_fps(60);
    player.open(&url)?;
    player.play();
    let start = Instant::now();
    let mut frame = None;
    let mut buffer = Vec::new();
    while window.is_open() && !window.is_key_down(Key::Escape) {
        let display_time = lychee::now_us() + 16_667;
        if player.has_new_frame(display_time) {
            frame = player.acquire_frame(display_time).or(frame);
        }
        let (width, height) = window.get_size();
        if width > 0 && height > 0 {
            buffer.resize(width * height, 0);
            buffer.fill(0);
            if let Some(frame) = &frame {
                draw(
                    frame,
                    player.video_size(),
                    player.rotation(),
                    &mut buffer,
                    width,
                    height,
                )?;
            }
            window.update_with_buffer(&buffer, width, height)?;
        } else {
            window.update();
        }
        if player.state() == State::Error {
            return Err(player
                .error()
                .unwrap_or_else(|| "playback failed".into())
                .into());
        }
        if player.state() == State::Ended || limit.is_some_and(|limit| start.elapsed() >= limit) {
            break;
        }
    }
    let stats = player.stats();
    println!(
        "summary: decoder={} hw={} rendered={} dropped={} position={:.3}",
        stats.video_decoder,
        stats.hardware_decoding,
        stats.frames_rendered,
        stats.frames_dropped,
        player.position().as_secs_f64()
    );
    Ok(())
}

// Nearest-neighbor scaling keeps the example independent of a graphics API.
fn draw(
    frame: &Frame,
    display_size: Option<(u32, u32)>,
    rotation: u32,
    output: &mut [u32],
    width: usize,
    height: usize,
) -> Result<(), Box<dyn std::error::Error>> {
    let (data, stride) = frame.data().ok_or("expected a CPU frame")?;
    let (source_w, source_h) = (frame.width() as usize, frame.height() as usize);
    let (display_w, display_h) = display_size.unwrap_or((frame.width(), frame.height()));
    let (display_w, display_h) = if rotation == 90 || rotation == 270 {
        (display_h as usize, display_w as usize)
    } else {
        (display_w as usize, display_h as usize)
    };
    let scale = (width as f64 / display_w as f64).min(height as f64 / display_h as f64);
    let (draw_w, draw_h) = (
        (display_w as f64 * scale).round().max(1.0) as usize,
        (display_h as f64 * scale).round().max(1.0) as usize,
    );
    let (left, top) = ((width - draw_w) / 2, (height - draw_h) / 2);
    let (rotated_w, rotated_h) = if rotation == 90 || rotation == 270 {
        (source_h, source_w)
    } else {
        (source_w, source_h)
    };
    for y in 0..draw_h {
        for x in 0..draw_w {
            let (rx, ry) = (x * rotated_w / draw_w, y * rotated_h / draw_h);
            let (sx, sy) = match rotation {
                90 => (ry, source_h - 1 - rx),
                180 => (source_w - 1 - rx, source_h - 1 - ry),
                270 => (source_w - 1 - ry, rx),
                _ => (rx, ry),
            };
            let offset = sy * stride + sx * 4;
            let pixel = &data[offset..offset + 4];
            output[(top + y) * width + left + x] =
                (u32::from(pixel[2]) << 16) | (u32::from(pixel[1]) << 8) | u32::from(pixel[0]);
        }
    }
    Ok(())
}
