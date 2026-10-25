//! Safe bindings to lychee_player's C ABI.
//!
//! The library plays files, network streams and pushed live input, and
//! never renders: once per vsync, ask [`Player::has_new_frame`] whether the
//! frame for that vsync's display time (the [`now_us`] timebase) changed,
//! and take it with [`Player::acquire_frame`]. A [`Frame`] is a CPU image
//! or a native handle (`CVPixelBufferRef`, `ID3D11Texture2D*`, a dmabuf),
//! released when dropped.

use std::ffi::{CStr, CString, c_char, c_void};
use std::fmt;
use std::ptr::{self, NonNull};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

pub use lychee_sys as sys;

/// Version of the C ABI the library implements (`LYCHEE_API_VERSION`).
pub fn api_version() -> i32 {
    unsafe { sys::lychee_api_version() }
}

/// The monotonic clock display times are in, in microseconds. On Apple
/// platforms this is `mach_absolute_time()`.
pub fn now_us() -> i64 {
    unsafe { sys::lychee_now_us() }
}

/// A request the library rejected, or arguments it cannot take (a string
/// with a NUL in it).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(&'static str);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "lychee: {}", self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

fn check(status: i32, what: &'static str) -> Result<()> {
    if status == 0 {
        Ok(())
    } else {
        Err(Error(what))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LogLevel {
    Error,
    Warning,
    Info,
    Debug,
    Verbose,
}

impl LogLevel {
    fn from_raw(level: i32) -> LogLevel {
        match level as u32 {
            sys::LYCHEE_LOG_ERROR => LogLevel::Error,
            sys::LYCHEE_LOG_WARNING => LogLevel::Warning,
            sys::LYCHEE_LOG_INFO => LogLevel::Info,
            sys::LYCHEE_LOG_DEBUG => LogLevel::Debug,
            _ => LogLevel::Verbose,
        }
    }
}

pub fn set_log_level(level: LogLevel) {
    let raw = match level {
        LogLevel::Error => sys::LYCHEE_LOG_ERROR,
        LogLevel::Warning => sys::LYCHEE_LOG_WARNING,
        LogLevel::Info => sys::LYCHEE_LOG_INFO,
        LogLevel::Debug => sys::LYCHEE_LOG_DEBUG,
        LogLevel::Verbose => sys::LYCHEE_LOG_VERBOSE,
    };
    unsafe { sys::lychee_set_log_level(raw as i32) }
}

type LogCallback = dyn Fn(LogLevel, &str) + Send + Sync;

// The library's log sink is global and may be called from any thread while
// it is replaced, so the C side always points at one trampoline, which
// takes the current callback out of this lock.
static LOG_CALLBACK: RwLock<Option<Arc<LogCallback>>> = RwLock::new(None);

/// Sends the library's log lines to `callback` (any thread) instead of
/// stderr; `None` restores stderr.
pub fn set_log_callback(callback: Option<Box<LogCallback>>) {
    let installed = callback.is_some();
    *LOG_CALLBACK.write().unwrap() = callback.map(Arc::from);
    let trampoline: sys::lychee_log_callback = if installed {
        Some(log_trampoline)
    } else {
        None
    };
    unsafe { sys::lychee_set_log_callback(trampoline, ptr::null_mut()) }
}

unsafe extern "C" fn log_trampoline(_: *mut c_void, level: i32, message: *const c_char) {
    let callback = LOG_CALLBACK.read().unwrap().clone();
    if let (Some(callback), false) = (callback, message.is_null()) {
        let message = unsafe { CStr::from_ptr(message) }.to_string_lossy();
        callback(LogLevel::from_raw(level), &message);
    }
}

/// What a frame holds; see the variants for what [`Frame::native_handle`]
/// points at.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FrameType {
    /// 8-bit BGRA in memory: [`Frame::data`].
    CpuBgra,
    /// 8-bit RGBA in memory: [`Frame::data`].
    CpuRgba,
    /// A `CVPixelBufferRef` (Apple), BGRA.
    CvPixelBuffer,
    /// Already rendered into the window given to
    /// [`Player::set_video_surface`] (Android); the frame only times it.
    AndroidSurface,
    /// An `ID3D11Texture2D*` on the device given to
    /// [`Player::set_video_device`] (Windows).
    D3d11Texture,
    /// A `*const lychee_sys::lychee_dmabuf` (Linux, VAAPI).
    Dmabuf,
}

impl FrameType {
    const ALL: [FrameType; 6] = [
        FrameType::CpuBgra,
        FrameType::CpuRgba,
        FrameType::CvPixelBuffer,
        FrameType::AndroidSurface,
        FrameType::D3d11Texture,
        FrameType::Dmabuf,
    ];

    fn bit(self) -> u32 {
        match self {
            FrameType::CpuBgra => sys::LYCHEE_FRAME_CPU_BGRA,
            FrameType::CpuRgba => sys::LYCHEE_FRAME_CPU_RGBA,
            FrameType::CvPixelBuffer => sys::LYCHEE_FRAME_CVPIXELBUFFER,
            FrameType::AndroidSurface => sys::LYCHEE_FRAME_ANDROID_SURFACE,
            FrameType::D3d11Texture => sys::LYCHEE_FRAME_D3D11_TEXTURE,
            FrameType::Dmabuf => sys::LYCHEE_FRAME_DMABUF,
        }
    }

    fn from_raw(raw: i32) -> Option<FrameType> {
        Self::ALL.into_iter().find(|t| t.bit() == raw as u32)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    /// The frame types the caller can show, in any order; the library picks
    /// per frame. Empty plays audio only.
    pub frame_types: Vec<FrameType>,
    /// Try hardware decoding first.
    pub hardware_decoding: bool,
    /// Android: have MediaCodec render [`FrameType::AndroidSurface`] frames
    /// upright, for windows that cannot rotate what they show.
    pub rotate_surface_frames: bool,
}

impl Default for Config {
    fn default() -> Config {
        Config {
            frame_types: vec![FrameType::CpuBgra],
            hardware_decoding: true,
            rotate_surface_frames: false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum State {
    Idle,
    /// Opening the input and filling buffers before the first frame.
    Loading,
    Ready,
    /// Waiting for data after a seek or a stall.
    Buffering,
    /// Played to the end.
    Ended,
    /// Final; [`Player::error`] says why.
    Error,
}

impl State {
    fn from_raw(raw: i32) -> State {
        match raw as u32 {
            sys::LYCHEE_STATE_LOADING => State::Loading,
            sys::LYCHEE_STATE_READY => State::Ready,
            sys::LYCHEE_STATE_BUFFERING => State::Buffering,
            sys::LYCHEE_STATE_ENDED => State::Ended,
            sys::LYCHEE_STATE_ERROR => State::Error,
            _ => State::Idle,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TrackType {
    Video,
    Audio,
    /// Text subtitles (SRT, WebVTT, mov_text, ASS/SSA) and bitmap
    /// subtitles (PGS, DVB, VobSub).
    Subtitle,
}

impl TrackType {
    fn raw(self) -> i32 {
        (match self {
            TrackType::Video => sys::LYCHEE_TRACK_VIDEO,
            TrackType::Audio => sys::LYCHEE_TRACK_AUDIO,
            TrackType::Subtitle => sys::LYCHEE_TRACK_SUBTITLE,
        }) as i32
    }

    fn from_raw(raw: i32) -> Option<TrackType> {
        match raw as u32 {
            sys::LYCHEE_TRACK_VIDEO => Some(TrackType::Video),
            sys::LYCHEE_TRACK_AUDIO => Some(TrackType::Audio),
            sys::LYCHEE_TRACK_SUBTITLE => Some(TrackType::Subtitle),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    StateChanged(State),
    /// Whether playback is requested ([`Player::play`] since the last pause).
    PlayingChanged(bool),
    /// Duration, tracks and video size are available.
    MediaInfo {
        duration: Option<Duration>,
    },
    /// Display size, before rotation.
    VideoSizeChanged {
        width: u32,
        height: u32,
    },
    /// The position the seek landed on.
    SeekCompleted(Duration),
    /// [`Player::error`] has the details.
    Error,
    /// A [`Player::select_track`] request was carried out.
    TracksChanged,
    /// The subtitle on screen changed: read its text with
    /// [`Player::subtitle`]. With `pictures`, it is drawn by
    /// [`Player::acquire_subtitle`] instead, text included.
    SubtitleChanged { pictures: bool },
}

impl Event {
    fn from_raw(kind: i32, arg0: i64, arg1: i64) -> Option<Event> {
        Some(match kind as u32 {
            sys::LYCHEE_EVENT_STATE_CHANGED => Event::StateChanged(State::from_raw(arg0 as i32)),
            sys::LYCHEE_EVENT_PLAYING_CHANGED => Event::PlayingChanged(arg0 != 0),
            sys::LYCHEE_EVENT_MEDIA_INFO => Event::MediaInfo {
                duration: duration(arg0),
            },
            sys::LYCHEE_EVENT_VIDEO_SIZE_CHANGED => Event::VideoSizeChanged {
                width: arg0 as u32,
                height: arg1 as u32,
            },
            sys::LYCHEE_EVENT_SEEK_COMPLETED => Event::SeekCompleted(micros(arg0)),
            sys::LYCHEE_EVENT_ERROR => Event::Error,
            sys::LYCHEE_EVENT_TRACKS_CHANGED => Event::TracksChanged,
            sys::LYCHEE_EVENT_SUBTITLE_CHANGED => Event::SubtitleChanged {
                pictures: arg1 != 0,
            },
            _ => return None,
        })
    }
}

fn micros(us: i64) -> Duration {
    Duration::from_micros(us.max(0) as u64)
}

fn duration(us: i64) -> Option<Duration> {
    (us >= 0).then(|| micros(us))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Track {
    /// The index for [`Player::select_track`].
    pub index: usize,
    pub kind: TrackType,
    /// The container's stream index.
    pub id: i32,
    pub selected: bool,
    pub codec: String,
    /// ISO 639 code from the container, or empty.
    pub language: String,
    pub title: String,
    /// Video: display size. Otherwise 0.
    pub width: u32,
    pub height: u32,
    /// Audio. Otherwise 0.
    pub sample_rate: u32,
    pub channels: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Stats {
    pub hardware_decoding: bool,
    pub frames_rendered: u64,
    pub frames_dropped: u64,
    pub video_decoder: String,
    /// Media read ahead of the playback position.
    pub video_buffered: Duration,
    pub audio_buffered: Duration,
    /// Moving average of the time decoding one video packet takes.
    pub video_decode_time: Duration,
}

/// Codecs of live tracks; see `lychee_codec` in `lychee.h` for the packet
/// and `config` formats.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Codec {
    /// Annex B access units, parameter sets in band.
    H264,
    Hevc,
    /// Raw access units; `config` is the AudioSpecificConfig.
    Aac,
    /// `config` is the ALAC magic cookie.
    Alac,
}

/// A live track for [`Player::set_live_track`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LiveTrack<'a> {
    /// [`TrackType::Video`] or [`TrackType::Audio`].
    pub kind: TrackType,
    /// `None` removes the track.
    pub codec: Option<Codec>,
    /// Video: the coded size if known, else 0.
    pub width: u32,
    pub height: u32,
    /// Audio.
    pub sample_rate: u32,
    pub channels: u32,
    /// Codec configuration; copied.
    pub config: &'a [u8],
}

type EventCallback = dyn Fn(Event) + Send + Sync;

// What the C side's user_data points at, for the player's whole life:
// replacing the callback swaps the Arc under the lock, and the trampoline
// calls a clone outside it.
struct EventSlot(Mutex<Option<Arc<EventCallback>>>);

unsafe extern "C" fn event_trampoline(user_data: *mut c_void, kind: i32, arg0: i64, arg1: i64) {
    let slot = unsafe { &*(user_data as *const EventSlot) };
    let callback = slot.0.lock().unwrap().clone();
    if let (Some(callback), Some(event)) = (callback, Event::from_raw(kind, arg0, arg1)) {
        callback(event);
    }
}

/// A player. Every method can be called from any thread.
pub struct Player {
    raw: NonNull<sys::lychee_player>,
    // Freed after lychee_player_destroy(), which joins the thread that
    // calls the trampoline.
    events: Box<EventSlot>,
}

// The C ABI is thread-safe.
unsafe impl Send for Player {}
unsafe impl Sync for Player {}

impl Player {
    pub fn new(config: &Config) -> Result<Player> {
        let mut raw_config = unsafe {
            let mut c = std::mem::MaybeUninit::<sys::lychee_config>::uninit();
            sys::lychee_config_init(c.as_mut_ptr());
            c.assume_init()
        };
        raw_config.video_frame_types = config.frame_types.iter().fold(0, |m, t| m | t.bit());
        raw_config.hardware_decoding = config.hardware_decoding as i32;
        raw_config.rotate_surface_frames = config.rotate_surface_frames as i32;
        let raw = NonNull::new(unsafe { sys::lychee_player_create(&raw_config) })
            .ok_or(Error("cannot create a player"))?;
        let events = Box::new(EventSlot(Mutex::new(None)));
        unsafe {
            sys::lychee_player_set_event_callback(
                raw.as_ptr(),
                Some(event_trampoline),
                &*events as *const EventSlot as *mut c_void,
            );
        }
        Ok(Player { raw, events })
    }

    fn raw(&self) -> *mut sys::lychee_player {
        self.raw.as_ptr()
    }

    /// Calls `callback` with every event, on the player's control thread,
    /// replacing the previous callback. Keep it short: hand events to your
    /// own thread (a channel).
    pub fn on_event(&self, callback: impl Fn(Event) + Send + Sync + 'static) {
        *self.events.0.lock().unwrap() = Some(Arc::new(callback));
    }

    /// Opens a path or URL asynchronously; progress arrives as
    /// [`Event::StateChanged`]. Opening again replaces what is open and
    /// keeps volume, rate, looping and play/pause.
    pub fn open(&self, url: &str) -> Result<()> {
        let url = CString::new(url).map_err(|_| Error("URL contains NUL"))?;
        check(
            unsafe { sys::lychee_player_open(self.raw(), url.as_ptr()) },
            "open rejected",
        )
    }

    /// Plays from the start again if the state is [`State::Ended`].
    pub fn play(&self) {
        unsafe { sys::lychee_player_play(self.raw()) }
    }

    pub fn pause(&self) {
        unsafe { sys::lychee_player_pause(self.raw()) }
    }

    pub fn seek(&self, position: Duration) {
        unsafe { sys::lychee_player_seek(self.raw(), position.as_micros() as i64) }
    }

    /// 0.0 - 1.0.
    pub fn set_volume(&self, volume: f32) {
        unsafe { sys::lychee_player_set_volume(self.raw(), volume) }
    }

    /// Clamped to 0.25 - 4.0. Audio keeps its pitch.
    pub fn set_rate(&self, rate: f64) {
        unsafe { sys::lychee_player_set_rate(self.raw(), rate) }
    }

    pub fn rate(&self) -> f64 {
        unsafe { sys::lychee_player_get_rate(self.raw()) }
    }

    /// At the end, play again from the start instead of ending.
    pub fn set_looping(&self, looping: bool) {
        unsafe { sys::lychee_player_set_looping(self.raw(), looping as i32) }
    }

    pub fn is_looping(&self) -> bool {
        unsafe { sys::lychee_player_is_looping(self.raw()) != 0 }
    }

    pub fn state(&self) -> State {
        State::from_raw(unsafe { sys::lychee_player_get_state(self.raw()) })
    }

    /// Whether playback is requested; stays true at [`State::Ended`].
    pub fn is_playing(&self) -> bool {
        unsafe { sys::lychee_player_is_playing(self.raw()) != 0 }
    }

    pub fn position(&self) -> Duration {
        micros(unsafe { sys::lychee_player_get_position(self.raw()) })
    }

    /// `None` if unknown (live streams).
    pub fn duration(&self) -> Option<Duration> {
        duration(unsafe { sys::lychee_player_get_duration(self.raw()) })
    }

    pub fn buffered_position(&self) -> Duration {
        micros(unsafe { sys::lychee_player_get_buffered_position(self.raw()) })
    }

    /// Display size, before rotation; `None` without video.
    pub fn video_size(&self) -> Option<(u32, u32)> {
        let (mut width, mut height) = (0, 0);
        unsafe { sys::lychee_player_get_video_size(self.raw(), &mut width, &mut height) };
        (width > 0 && height > 0).then_some((width as u32, height as u32))
    }

    /// Clockwise rotation to apply when showing frames: 0, 90, 180 or 270.
    pub fn rotation(&self) -> u32 {
        unsafe { sys::lychee_player_get_video_rotation(self.raw()) as u32 }
    }

    pub fn error(&self) -> Option<String> {
        read_string(|buffer, size| unsafe {
            sys::lychee_player_get_error(self.raw(), buffer, size)
        })
    }

    /// Available from [`Event::MediaInfo`] on.
    pub fn tracks(&self) -> Vec<Track> {
        let count = unsafe { sys::lychee_player_get_track_count(self.raw()) };
        (0..count).filter_map(|index| self.track(index)).collect()
    }

    fn track(&self, index: i32) -> Option<Track> {
        let mut info: sys::lychee_track_info = unsafe { std::mem::zeroed() };
        info.struct_size = size_of::<sys::lychee_track_info>() as u32;
        if unsafe { sys::lychee_player_get_track_info(self.raw(), index, &mut info) } != 0 {
            return None;
        }
        Some(Track {
            index: index as usize,
            kind: TrackType::from_raw(info.type_)?,
            id: info.id,
            selected: info.selected != 0,
            codec: c_array(&info.codec),
            language: c_array(&info.language),
            title: c_array(&info.title),
            width: info.width as u32,
            height: info.height as u32,
            sample_rate: info.sample_rate as u32,
            channels: info.channels as u32,
        })
    }

    /// Switches the `kind` track to track `index` (a [`Track::index`]), or
    /// turns `kind` off with `None`. [`Event::TracksChanged`] follows.
    pub fn select_track(&self, kind: TrackType, index: Option<usize>) -> Result<()> {
        let index = index.map_or(-1, |i| i as i32);
        check(
            unsafe { sys::lychee_player_select_track(self.raw(), kind.raw(), index) },
            "track selection rejected",
        )
    }

    /// The subtitle text showing at `display_time_us` ([`now_us`]
    /// timebase; 0 for now), lines separated by `'\n'`.
    pub fn subtitle(&self, display_time_us: i64) -> Option<String> {
        read_string(|buffer, size| unsafe {
            sys::lychee_player_get_subtitle(self.raw(), display_time_us, buffer, size)
        })
    }

    /// The subtitle pictures showing at `display_time_us` ([`now_us`]
    /// timebase; 0 for now), laid out on a `width` x `height` canvas the
    /// video fills (0 x 0: the video's size), or `None` if there are none.
    /// Call it every frame while [`Event::SubtitleChanged`] said
    /// `pictures`, and redraw when [`Subtitle::id`] changes.
    pub fn acquire_subtitle(&self, display_time_us: i64, width: u32, height: u32) -> Option<Subtitle> {
        let raw = unsafe {
            sys::lychee_player_acquire_subtitle(self.raw(), display_time_us, width as i32, height as i32)
        };
        NonNull::new(raw).map(|raw| Subtitle { raw })
    }

    /// The container tag `key` ("title", "artist", "album", ...).
    pub fn metadata(&self, key: &str) -> Option<String> {
        let key = CString::new(key).ok()?;
        read_string(|buffer, size| unsafe {
            sys::lychee_player_get_metadata(self.raw(), key.as_ptr(), buffer, size)
        })
    }

    /// The cover picture as the container stores it, usually JPEG or PNG.
    pub fn artwork(&self) -> Option<Vec<u8>> {
        let size = unsafe {
            sys::lychee_player_get_artwork(self.raw(), std::ptr::null_mut(), 0)
        };
        if size < 0 {
            return None;
        }
        let mut artwork = vec![0u8; size as usize];
        let copied = unsafe {
            sys::lychee_player_get_artwork(self.raw(), artwork.as_mut_ptr(), size)
        };
        // A reopen in between can change the picture.
        artwork.truncate(copied.clamp(0, size) as usize);
        Some(artwork)
    }

    pub fn stats(&self) -> Stats {
        let mut stats: sys::lychee_stats = unsafe { std::mem::zeroed() };
        stats.struct_size = size_of::<sys::lychee_stats>() as u32;
        unsafe { sys::lychee_player_get_stats(self.raw(), &mut stats) };
        Stats {
            hardware_decoding: stats.hardware_decoding != 0,
            frames_rendered: stats.frames_rendered as u64,
            frames_dropped: stats.frames_dropped as u64,
            video_decoder: c_array(&stats.video_decoder),
            video_buffered: micros(stats.video_buffered_us),
            audio_buffered: micros(stats.audio_buffered_us),
            video_decode_time: micros(stats.video_decode_time_us),
        }
    }

    /// Whether the frame for `display_time_us` ([`now_us`] timebase)
    /// differs from the last one acquired. Call it once per vsync with that
    /// vsync's display time, even when you do not redraw: the library
    /// estimates the vsync interval from these calls.
    pub fn has_new_frame(&self, display_time_us: i64) -> bool {
        unsafe { sys::lychee_player_has_new_frame(self.raw(), display_time_us) != 0 }
    }

    /// The frame to show at `display_time_us`, or `None` if there is none
    /// yet. It may outlive the player.
    pub fn acquire_frame(&self, display_time_us: i64) -> Option<Frame> {
        let raw = unsafe { sys::lychee_player_acquire_frame(self.raw(), display_time_us) };
        NonNull::new(raw).map(|raw| Frame { raw })
    }

    /// Stops what is open and plays packets pushed with
    /// [`Player::push_live_packet`] as they arrive, at least `latency`
    /// after they do (zero for 80 ms).
    pub fn open_live(&self, latency: Duration) -> Result<()> {
        let config = sys::lychee_live_config {
            struct_size: size_of::<sys::lychee_live_config>() as u32,
            latency_us: latency.as_micros() as i64,
        };
        check(
            unsafe { sys::lychee_player_open_live(self.raw(), &config) },
            "live open rejected",
        )
    }

    /// Declares, changes or removes a live track. Packets pushed afterwards
    /// belong to the new codec.
    pub fn set_live_track(&self, track: &LiveTrack<'_>) -> Result<()> {
        let codec = match track.codec {
            None => sys::LYCHEE_CODEC_NONE,
            Some(Codec::H264) => sys::LYCHEE_CODEC_H264,
            Some(Codec::Hevc) => sys::LYCHEE_CODEC_HEVC,
            Some(Codec::Aac) => sys::LYCHEE_CODEC_AAC,
            Some(Codec::Alac) => sys::LYCHEE_CODEC_ALAC,
        };
        let raw = sys::lychee_live_track {
            struct_size: size_of::<sys::lychee_live_track>() as u32,
            type_: track.kind.raw(),
            codec: codec as i32,
            width: track.width as i32,
            height: track.height as i32,
            sample_rate: track.sample_rate as i32,
            channels: track.channels as i32,
            config: track.config.as_ptr(),
            config_size: track.config.len() as i32,
        };
        check(
            unsafe { sys::lychee_player_set_live_track(self.raw(), &raw) },
            "live track rejected",
        )
    }

    /// Queues one packet of a declared live track. `pts_us` is on the
    /// sender's clock, the same for audio and video; `data` is copied.
    pub fn push_live_packet(&self, kind: TrackType, pts_us: i64, data: &[u8]) -> Result<()> {
        let packet = sys::lychee_live_packet {
            struct_size: size_of::<sys::lychee_live_packet>() as u32,
            type_: kind.raw(),
            pts_us,
            data: data.as_ptr(),
            size: data.len() as i32,
        };
        check(
            unsafe { sys::lychee_player_push_live_packet(self.raw(), &packet) },
            "live packet rejected",
        )
    }

    /// Drops what was pushed of `kind` (`None` for both) and not played yet.
    pub fn flush_live(&self, kind: Option<TrackType>) -> Result<()> {
        let kind = kind.map_or(0, TrackType::raw);
        check(
            unsafe { sys::lychee_player_flush_live(self.raw(), kind) },
            "live flush rejected",
        )
    }

    /// Changes the video device frames are decoded on, or none.
    ///
    /// # Safety
    ///
    /// `device` must be a valid `lychee_video_device` (`struct_size` set)
    /// whose handle is valid for the device's type.
    pub unsafe fn set_video_device(&self, device: Option<&sys::lychee_video_device>) -> Result<()> {
        let device = device.map_or(ptr::null(), |d| d as *const _);
        check(
            unsafe { sys::lychee_player_set_video_device(self.raw(), device) },
            "video device not supported",
        )
    }

    /// Android: the `ANativeWindow*` that [`FrameType::AndroidSurface`]
    /// frames are rendered into, or null to detach it.
    ///
    /// # Safety
    ///
    /// `surface` must be null or a valid `ANativeWindow*`; the library takes
    /// its own reference.
    pub unsafe fn set_video_surface(&self, surface: *mut c_void) {
        unsafe { sys::lychee_player_set_video_surface(self.raw(), surface) }
    }
}

impl Drop for Player {
    fn drop(&mut self) {
        // Joins the player's threads: no event is delivered after this.
        unsafe { sys::lychee_player_destroy(self.raw()) }
    }
}

/// A decoded frame, released when dropped.
/// Subtitle pictures from [`Player::acquire_subtitle`]. They may outlive
/// the player.
pub struct Subtitle {
    raw: NonNull<sys::lychee_subtitle>,
}

// Immutable; owns a reference to its pictures.
unsafe impl Send for Subtitle {}
unsafe impl Sync for Subtitle {}

/// One picture of a [`Subtitle`].
#[derive(Debug, Clone, Copy)]
pub struct SubtitleImage<'a> {
    /// Where to draw it on the canvas.
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    /// Premultiplied RGBA, `stride` bytes a row, scaled to `width` x
    /// `height` when drawn.
    pub bitmap_width: u32,
    pub bitmap_height: u32,
    pub stride: usize,
    pub data: &'a [u8],
}

impl Subtitle {
    /// The same for the same pictures, different otherwise.
    pub fn id(&self) -> i64 {
        unsafe { sys::lychee_subtitle_get_id(self.raw.as_ptr()) }
    }

    pub fn images(&self) -> Vec<SubtitleImage<'_>> {
        let count = unsafe { sys::lychee_subtitle_get_image_count(self.raw.as_ptr()) };
        (0..count)
            .filter_map(|i| {
                let mut image: sys::lychee_subtitle_image = unsafe { std::mem::zeroed() };
                image.struct_size = size_of::<sys::lychee_subtitle_image>() as u32;
                if unsafe { sys::lychee_subtitle_get_image(self.raw.as_ptr(), i, &mut image) } != 0 {
                    return None;
                }
                let len = image.stride as usize * image.bitmap_height as usize;
                Some(SubtitleImage {
                    x: image.x,
                    y: image.y,
                    width: image.width as u32,
                    height: image.height as u32,
                    bitmap_width: image.bitmap_width as u32,
                    bitmap_height: image.bitmap_height as u32,
                    stride: image.stride as usize,
                    data: unsafe { std::slice::from_raw_parts(image.data, len) },
                })
            })
            .collect()
    }
}

impl Drop for Subtitle {
    fn drop(&mut self) {
        unsafe { sys::lychee_subtitle_release(self.raw.as_ptr()) }
    }
}

impl fmt::Debug for Subtitle {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Subtitle").field("id", &self.id()).finish()
    }
}

pub struct Frame {
    raw: NonNull<sys::lychee_frame>,
}

// A frame is immutable and owns a reference to its picture.
unsafe impl Send for Frame {}
unsafe impl Sync for Frame {}

impl Frame {
    fn raw(&self) -> *const sys::lychee_frame {
        self.raw.as_ptr()
    }

    /// `None` for types newer than this crate.
    pub fn kind(&self) -> Option<FrameType> {
        FrameType::from_raw(unsafe { sys::lychee_frame_get_type(self.raw()) })
    }

    pub fn width(&self) -> u32 {
        unsafe { sys::lychee_frame_get_width(self.raw()) as u32 }
    }

    pub fn height(&self) -> u32 {
        unsafe { sys::lychee_frame_get_height(self.raw()) as u32 }
    }

    /// Presentation time: the media position, or for live input the time
    /// in the [`now_us`] timebase.
    pub fn pts_us(&self) -> i64 {
        unsafe { sys::lychee_frame_get_pts(self.raw()) }
    }

    /// CPU frames: the pixels, `height` rows of `stride` bytes.
    pub fn data(&self) -> Option<(&[u8], usize)> {
        let mut stride = 0;
        let data = unsafe { sys::lychee_frame_get_data(self.raw(), &mut stride) };
        if data.is_null() || stride <= 0 {
            return None;
        }
        let len = stride as usize * self.height() as usize;
        Some((
            unsafe { std::slice::from_raw_parts(data, len) },
            stride as usize,
        ))
    }

    /// The native handle ([`FrameType`] says which); not retained, so
    /// retain it yourself if it must outlive the frame.
    pub fn native_handle(&self) -> *mut c_void {
        unsafe { sys::lychee_frame_get_native_handle(self.raw()) }
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe { sys::lychee_frame_release(self.raw.as_ptr()) }
    }
}

impl fmt::Debug for Frame {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Frame")
            .field("kind", &self.kind())
            .field("width", &self.width())
            .field("height", &self.height())
            .field("pts_us", &self.pts_us())
            .finish()
    }
}

// Reads a string from a C function that copies it into a buffer and returns
// its full length (negative or 0: none), growing the buffer once if needed.
fn read_string(read: impl Fn(*mut c_char, i32) -> i32) -> Option<String> {
    let mut buffer = vec![0u8; 256];
    let mut len = read(buffer.as_mut_ptr().cast(), buffer.len() as i32);
    if len as usize >= buffer.len() {
        buffer.resize(len as usize + 1, 0);
        len = read(buffer.as_mut_ptr().cast(), buffer.len() as i32);
    }
    if len <= 0 {
        return None;
    }
    buffer.truncate((len as usize).min(buffer.len() - 1));
    Some(String::from_utf8_lossy(&buffer).into_owned())
}

fn c_array(chars: &[c_char]) -> String {
    let bytes: Vec<u8> = chars
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}
