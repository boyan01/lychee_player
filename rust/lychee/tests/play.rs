// Plays tool/regression.sh's media (run it once to create
// /tmp/lychee-media) through the safe API.

use std::sync::Arc;
use std::sync::mpsc::{self, Receiver};
use std::time::{Duration, Instant};

use lychee::{Config, Event, FrameType, Player, State, TrackType};

const MEDIA: &str = "/tmp/lychee-media/h264_aac.mp4";

fn open() -> (Player, Receiver<Event>) {
    open_file(MEDIA)
}

fn open_file(path: &str) -> (Player, Receiver<Event>) {
    assert!(
        std::path::Path::new(path).exists(),
        "{path} is missing: run tool/regression.sh once"
    );
    let player = Player::new(&Config::default()).unwrap();
    let (events, received) = mpsc::channel();
    player.on_event(move |event| {
        let _ = events.send(event);
    });
    player.set_volume(0.0);
    player.open(path).unwrap();
    (player, received)
}

// Pulls frames as a 60 Hz display would, for at most `duration` or until an
// event matches `until`. Returns the frames acquired and whether one did.
fn pull(
    player: &Player,
    events: &Receiver<Event>,
    duration: Duration,
    mut until: impl FnMut(&Event) -> bool,
) -> (usize, bool) {
    let deadline = Instant::now() + duration;
    let mut frames = 0;
    while Instant::now() < deadline {
        if events.try_iter().any(|event| until(&event)) {
            return (frames, true);
        }
        let display_time = lychee::now_us() + 16_667;
        if player.has_new_frame(display_time) {
            let frame = player.acquire_frame(display_time).unwrap();
            assert_eq!(frame.kind(), Some(FrameType::CpuBgra));
            let (data, stride) = frame.data().unwrap();
            assert!(stride >= frame.width() as usize * 4);
            assert_eq!(data.len(), stride * frame.height() as usize);
            frames += 1;
        }
        std::thread::sleep(Duration::from_millis(16));
    }
    (frames, false)
}

fn wait_for(player: &Player, events: &Receiver<Event>, until: impl FnMut(&Event) -> bool) {
    let (_, seen) = pull(player, events, Duration::from_secs(10), until);
    assert!(
        seen,
        "timed out in {:?}: {:?}",
        player.state(),
        player.error()
    );
}

#[test]
fn plays_seeks_and_ends() {
    let (player, events) = open();
    player.play();
    wait_for(&player, &events, |e| {
        *e == Event::StateChanged(State::Ready)
    });

    assert_eq!(player.duration().map(|d| d.as_secs()), Some(12));
    assert_eq!(player.video_size(), Some((1920, 1080)));
    assert_eq!(player.artwork(), None);
    let tracks = player.tracks();
    assert!(
        tracks
            .iter()
            .any(|t| t.kind == TrackType::Video && t.selected),
        "{tracks:?}"
    );
    assert!(
        tracks
            .iter()
            .any(|t| t.kind == TrackType::Audio && t.selected),
        "{tracks:?}"
    );

    // 30 fps media: about 30 new frames in a second.
    let (frames, _) = pull(&player, &events, Duration::from_secs(1), |_| false);
    assert!(frames >= 20, "{frames} frames in a second");

    player.seek(Duration::from_secs(10));
    wait_for(&player, &events, |e| matches!(e, Event::SeekCompleted(_)));
    wait_for(&player, &events, |e| {
        *e == Event::StateChanged(State::Ended)
    });
    let stats = player.stats();
    assert!(stats.frames_rendered > 0, "{stats:?}");
    assert_eq!(player.error(), None);
}

#[test]
fn destroying_frees_the_callback_and_frames_outlive_it() {
    let (player, events) = open();
    let token = Arc::new(());
    let held = token.clone();
    player.on_event(move |_| {
        let _ = &held;
    });
    player.play();
    drop(events);

    let deadline = Instant::now() + Duration::from_secs(10);
    let frame = loop {
        assert!(Instant::now() < deadline, "no frame: {:?}", player.error());
        let display_time = lychee::now_us() + 16_667;
        if player.has_new_frame(display_time)
            && let Some(frame) = player.acquire_frame(display_time)
        {
            break frame;
        }
        std::thread::sleep(Duration::from_millis(16));
    };

    // Destroying the player joins its threads and frees the callback; the
    // frame keeps its own picture.
    drop(player);
    assert_eq!(Arc::strong_count(&token), 1);
    assert!(frame.data().is_some());
}

#[test]
fn subtitle_pictures() {
    // A PGS box at 660,900, 600x80 on a 1920x1080 canvas, from 1 s.
    let (player, events) = open_file("/tmp/lychee-media/sub_pgs.mkv");
    player.play();
    wait_for(&player, &events, |e| {
        *e == Event::SubtitleChanged { pictures: true }
    });
    let subtitle = player.acquire_subtitle(0, 960, 540).unwrap();
    let images = subtitle.images();
    assert_eq!(images.len(), 1);
    let image = images[0];
    assert_eq!((image.x, image.y, image.width, image.height), (330, 450, 300, 40));
    assert_eq!((image.bitmap_width, image.bitmap_height), (600, 80));
    assert_eq!(image.data.len(), image.stride * 80);
    // Black border, white inside, opaque.
    assert_eq!(&image.data[..4], &[0, 0, 0, 255]);
    let middle = image.stride * 40 + 300 * 4;
    assert_eq!(&image.data[middle..middle + 4], &[255, 255, 255, 255]);
    let again = player.acquire_subtitle(0, 960, 540).unwrap();
    assert_eq!(again.id(), subtitle.id());
}
