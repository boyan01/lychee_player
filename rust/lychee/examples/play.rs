//! Plays a file or URL in a desktop window using CPU frames.
//!
//!     cargo run --example play -- [--seconds N] [--key SEC:NAME] URL [FRAME.ppm]

use std::time::{Duration, Instant};

use lychee::{Config, Frame, Player, State};
use minifb::{Key, KeyRepeat, MouseButton, MouseMode, Window, WindowOptions};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let mut limit = None;
    let mut scheduled = Vec::new();
    let url = loop {
        match args.next().as_deref() {
            Some("--seconds") => {
                let seconds: f64 = args.next().ok_or("missing seconds")?.parse()?;
                if !seconds.is_finite() || seconds <= 0.0 {
                    return Err("seconds must be positive and finite".into());
                }
                limit = Some(Duration::try_from_secs_f64(seconds)?);
            }
            Some("--key") => {
                let value = args.next().ok_or("missing key")?;
                let (time, name) = value.split_once(':').ok_or("expected SEC:NAME")?;
                let time: f64 = time.parse()?;
                let time = Duration::try_from_secs_f64(time)?;
                let action = match name {
                    "UP" => Action::Key(Key::Up),
                    "DOWN" => Action::Key(Key::Down),
                    "LEFT" => Action::Key(Key::Left),
                    "RIGHT" => Action::Key(Key::Right),
                    "SPACE" => Action::Key(Key::Space),
                    "M" => Action::Key(Key::M),
                    _ if name.starts_with("CLICK=") => {
                        let percent: f64 = name[6..].parse()?;
                        if !percent.is_finite() || !(0.0..=100.0).contains(&percent) {
                            return Err("click percentage must be between 0 and 100".into());
                        }
                        Action::Click(percent / 100.0)
                    }
                    _ => return Err("unknown key".into()),
                };
                scheduled.push((time, action));
            }
            Some(url) => break url.to_owned(),
            None => return Err("usage: play [--seconds N] [--key SEC:NAME] URL [FRAME.ppm]".into()),
        }
    };
    scheduled.sort_by_key(|(time, _)| *time);
    let mut scheduled = scheduled.into_iter().peekable();
    let snapshot = args.next();
    if args.next().is_some() {
        return Err("usage: play [--seconds N] URL [FRAME.ppm]".into());
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
    let mut wrote_snapshot = snapshot.is_none();
    let mut buffer = Vec::new();
    let mut volume = 1.0_f32;
    let mut muted = false;
    let mut right_was_down = false;
    while window.is_open() && !window.is_key_down(Key::Escape) {
        let display_time = lychee::now_us() + 16_667;
        if player.has_new_frame(display_time)
            && let Some(next) = player.acquire_frame(display_time)
        {
            if !wrote_snapshot && next.pts_us() >= 2_000_000 {
                let path = snapshot.as_deref().unwrap();
                write_ppm(&next, path)?;
                println!("wrote {path} ({next:?})");
                wrote_snapshot = true;
            }
            frame = Some(next);
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
        let mut actions = Vec::new();
        for (key, repeat) in [
            (Key::Space, KeyRepeat::No),
            (Key::M, KeyRepeat::No),
            (Key::Up, KeyRepeat::Yes),
            (Key::Down, KeyRepeat::Yes),
            (Key::Left, KeyRepeat::Yes),
            (Key::Right, KeyRepeat::Yes),
        ] {
            if window.is_key_pressed(key, repeat) {
                actions.push(Action::Key(key));
            }
        }
        let right_down = window.get_mouse_down(MouseButton::Right);
        if right_down
            && !right_was_down
            && width > 1
            && let Some((x, _)) = window.get_mouse_pos(MouseMode::Discard)
        {
            actions.push(Action::Click(
                (f64::from(x) / (width - 1) as f64).clamp(0.0, 1.0),
            ));
        }
        right_was_down = right_down;
        while scheduled
            .peek()
            .is_some_and(|(time, _)| start.elapsed() >= *time)
        {
            actions.push(scheduled.next().unwrap().1);
        }
        for action in actions {
            if matches!(action, Action::Key(Key::Space)) {
                if player.is_playing() {
                    player.pause();
                    println!("paused");
                } else {
                    player.play();
                    println!("playing");
                }
            }
            if matches!(action, Action::Key(Key::M)) {
                muted = !muted;
                player.set_volume(if muted { 0.0 } else { volume });
                println!("{}", if muted { "muted" } else { "unmuted" });
            }
            for (key, delta) in [(Key::Up, 0.1), (Key::Down, -0.1)] {
                if matches!(action, Action::Key(k) if k == key) {
                    volume = (volume + delta).clamp(0.0, 1.0);
                    muted = false;
                    player.set_volume(volume);
                    println!("volume: {:.0}%", volume * 100.0);
                }
            }
            for (key, forward) in [(Key::Left, false), (Key::Right, true)] {
                if matches!(action, Action::Key(k) if k == key) && player.is_seekable() {
                    let position = player.position();
                    let step = Duration::from_secs(5);
                    let target = if forward {
                        let target = position.saturating_add(step);
                        player
                            .duration()
                            .map_or(target, |duration| target.min(duration))
                    } else {
                        position.saturating_sub(step)
                    };
                    player.seek(target);
                    println!("seek: {:.2}s", target.as_secs_f64());
                }
            }
            if let Action::Click(fraction) = action
                && player.is_seekable()
                && let Some(duration) = player.duration()
            {
                let target = duration.mul_f64(fraction);
                player.seek(target);
                println!(
                    "seek: {:.2}s ({:.0}%)",
                    target.as_secs_f64(),
                    fraction * 100.0
                );
            }
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

enum Action {
    Key(Key),
    Click(f64),
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

fn write_ppm(frame: &lychee::Frame, path: &str) -> std::io::Result<()> {
    let (data, stride) = frame.data().ok_or(std::io::ErrorKind::Unsupported)?;
    let (width, height) = (frame.width() as usize, frame.height() as usize);
    let (r, b) = match frame.kind() {
        Some(lychee::FrameType::CpuRgba) => (0, 2),
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
