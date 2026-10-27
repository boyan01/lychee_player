import CoreVideo
import LycheePlayer
import XCTest

// Needs the media tool/regression.sh generates in /tmp/lychee-media.
final class LycheePlayerTests: XCTestCase {
  private let url = "/tmp/lychee-media/h264_aac.mp4"

  override func setUpWithError() throws {
    try XCTSkipUnless(FileManager.default.fileExists(atPath: url), "run tool/regression.sh first")
  }

  // `swift test --traits NoFFmpeg` runs the core without FFmpeg, which has
  // no bitmap subtitle decoder and reads no bare MP3 files.
  private func skipWithoutFFmpeg(_ what: String) throws {
    #if NoFFmpeg
    throw XCTSkip("\(what) needs the FFmpeg core")
    #endif
  }

  func testPlaysThroughFrameDriver() throws {
    let player = LycheePlayer()
    player.volume = 0
    let ready = expectation(description: "ready")
    player.onEvent = { event in
      if case .stateChanged(.ready) = event { ready.fulfill() }
    }
    XCTAssertFalse(player.isSeekable)
    player.open(url)
    player.play()
    wait(for: [ready], timeout: 10)
    XCTAssertTrue(player.isSeekable)
    XCTAssertEqual(player.duration, 12)
    XCTAssertNil(player.artwork)
    XCTAssertEqual(player.videoSize, CGSize(width: 1920, height: 1080))
    XCTAssertEqual(player.tracks.map(\.type), [.video, .audio])

    var frames = 0
    var size = CGSize.zero
    let pulled = expectation(description: "frames")
    var driver: LycheeFrameDriver?
    driver = LycheeFrameDriver(player: player) {
      guard let frame = driver?.copyFrame() else { return }
      size = CGSize(width: CVPixelBufferGetWidth(frame), height: CVPixelBufferGetHeight(frame))
      frames += 1
      if frames == 30 { pulled.fulfill() }
    }
    driver?.start()
    wait(for: [pulled], timeout: 10)
    XCTAssertEqual(size, CGSize(width: 1920, height: 1080))
    driver?.stop()
    XCTAssertNil(driver?.copyFrame())
    XCTAssertEqual(player.stats.framesDropped, 0)
  }

  func testRebuildKeepsPausedPlayerPaused() {
    let player = LycheePlayer()
    let ready = expectation(description: "ready")
    player.onEvent = { event in
      if case .stateChanged(.ready) = event { ready.fulfill() }
    }
    player.open(url)
    wait(for: [ready], timeout: 10)
    let position = player.position
    let rebuilt = expectation(description: "rebuilt")
    player.onEvent = { event in
      if case .stateChanged(.ready) = event { rebuilt.fulfill() }
    }
    player.rebuildVideoDecoder()
    wait(for: [rebuilt], timeout: 10)
    XCTAssertFalse(player.isPlaying)
    XCTAssertEqual(player.position, position, accuracy: 0.001)
    XCTAssertEqual(player.state, .ready)
  }

  func testReleasedWhileDriverRunning() {
    weak var weakPlayer: LycheePlayer?
    var driver: LycheeFrameDriver?
    do {
      let player = LycheePlayer()
      weakPlayer = player
      player.open(url)
      driver = LycheeFrameDriver(player: player) {}
      driver?.start()
    }
    XCTAssertNotNil(weakPlayer, "the driver keeps the player")
    driver?.stop()
    XCTAssertNil(weakPlayer, "stop() lets go of it")
    driver = nil
  }

  func testArtwork() {
    let player = LycheePlayer()
    let loaded = expectation(description: "media info")
    player.onEvent = { event in
      if case .mediaInfo = event { loaded.fulfill() }
    }
    player.open("/tmp/lychee-media/artwork.m4a")
    wait(for: [loaded], timeout: 10)
    XCTAssertEqual(player.artwork?.prefix(4), Data([0x89, 0x50, 0x4e, 0x47]), "PNG")
  }

  func testSubtitlePictures() throws {
    try skipWithoutFFmpeg("PGS")
    // A PGS box at 660,900, 600x80 on a 1920x1080 canvas, from 1 s.
    let player = LycheePlayer()
    player.volume = 0
    let shown = expectation(description: "pictures")
    player.onEvent = { event in
      if case .subtitleChanged(pictures: true) = event { shown.fulfill() }
    }
    player.open("/tmp/lychee-media/sub_pgs.mkv")
    player.play()
    var driver: LycheeFrameDriver?
    driver = LycheeFrameDriver(player: player) { _ = driver?.copyFrame() }
    driver?.start()
    wait(for: [shown], timeout: 10)
    driver?.stop()
    let subtitle = player.subtitlePictures(width: 960, height: 540)
    XCTAssertEqual(subtitle?.pictures.count, 1)
    XCTAssertEqual(subtitle?.pictures.first?.rect, CGRect(x: 330, y: 450, width: 300, height: 40))
    XCTAssertEqual(subtitle?.pictures.first?.image.width, 600)
    XCTAssertEqual(player.subtitlePictures(width: 960, height: 540)?.id, subtitle?.id)
  }

  // The view's pictures follow subtitle events, not only new frames: turned
  // off while paused, they go at once. The app's onEvent still gets them.
  func testViewClearsSubtitleWhilePaused() throws {
    try skipWithoutFFmpeg("PGS")
    let player = LycheePlayer()
    player.volume = 0
    let view = LycheePlayerView(frame: CGRect(x: 0, y: 0, width: 960, height: 540))
    view.player = player
    let shown = expectation(description: "pictures")
    let cleared = expectation(description: "none")
    var paused = false
    player.onEvent = { event in
      guard case .subtitleChanged(let pictures) = event else { return }
      if pictures && !paused { shown.fulfill() }
      if !pictures && paused { cleared.fulfill() }
    }
    player.open("/tmp/lychee-media/sub_pgs.mkv")
    player.play()
    wait(for: [shown], timeout: 10)
    #if os(macOS)
    let subtitleLayer = try XCTUnwrap(view.layer?.sublayers?.last)
    #else
    let subtitleLayer = try XCTUnwrap(view.layer.sublayers?.last)
    #endif
    XCTAssertEqual(subtitleLayer.sublayers?.count, 1)

    player.pause()
    paused = true
    XCTAssertTrue(player.selectTrack(.subtitle, index: nil))
    wait(for: [cleared], timeout: 10)
    XCTAssertEqual(subtitleLayer.sublayers?.count ?? 0, 0)
  }

  // Opening media without video on the same player takes the last frame
  // off the screen: no new frame would replace it.
  func testViewClearsFrameForMediaWithoutVideo() throws {
    try skipWithoutFFmpeg("MP3")
    let player = LycheePlayer()
    player.volume = 0
    let view = LycheePlayerView(frame: CGRect(x: 0, y: 0, width: 480, height: 270))
    // The view pulls frames only in a window.
    #if os(macOS)
    let window = NSWindow(contentRect: view.frame, styleMask: .borderless, backing: .buffered,
                          defer: false)
    window.contentView = view
    window.orderFront(nil)
    defer { window.orderOut(nil) }
    let videoLayer = try XCTUnwrap(view.layer?.sublayers?.first)
    #else
    let window = UIWindow(frame: view.frame)
    window.addSubview(view)
    window.isHidden = false
    let videoLayer = try XCTUnwrap(view.layer.sublayers?.first)
    #endif
    view.player = player
    player.open(url)
    player.play()
    let shown = expectation(for: NSPredicate { _, _ in videoLayer.contents != nil }, evaluatedWith: nil)
    wait(for: [shown], timeout: 10)

    let loaded = expectation(description: "media info")
    player.onEvent = { event in
      if case .mediaInfo = event { loaded.fulfill() }
    }
    player.open("/tmp/lychee-media/audio_only.mp3")
    wait(for: [loaded], timeout: 10)
    XCTAssertNil(player.videoSize)
    XCTAssertNil(videoLayer.contents)
  }

  // A player prepared before it is attached sends no new media event, so
  // replacing the view's player must clear the old picture immediately.
  func testViewClearsFrameWhenReplacedWithReadyAudioPlayer() throws {
    try skipWithoutFFmpeg("MP3")
    let player = LycheePlayer()
    player.volume = 0
    let view = LycheePlayerView(frame: CGRect(x: 0, y: 0, width: 480, height: 270))
    #if os(macOS)
    let window = NSWindow(contentRect: view.frame, styleMask: .borderless, backing: .buffered,
                          defer: false)
    window.contentView = view
    window.orderFront(nil)
    defer { window.orderOut(nil) }
    let videoLayer = try XCTUnwrap(view.layer?.sublayers?.first)
    #else
    let window = UIWindow(frame: view.frame)
    window.addSubview(view)
    window.isHidden = false
    let videoLayer = try XCTUnwrap(view.layer.sublayers?.first)
    #endif
    view.player = player
    defer { view.player = nil }
    player.open(url)
    player.play()
    let shown = expectation(for: NSPredicate { _, _ in videoLayer.contents != nil }, evaluatedWith: nil)
    wait(for: [shown], timeout: 10)

    let audio = LycheePlayer()
    audio.volume = 0
    let ready = expectation(description: "audio ready")
    audio.onEvent = { event in
      if case .stateChanged(.ready) = event { ready.fulfill() }
    }
    audio.open("/tmp/lychee-media/audio_only.mp3")
    wait(for: [ready], timeout: 10)
    XCTAssertNil(audio.videoSize)

    view.player = audio
    XCTAssertNil(videoLayer.contents)
  }

  func testHDRFramesStay10Bit() {
    let player = LycheePlayer()
    player.volume = 0
    player.open("/tmp/lychee-media/hdr_pq.mp4")
    player.play()
    var frame: CVPixelBuffer?
    let pulled = expectation(description: "frame")
    var driver: LycheeFrameDriver?
    driver = LycheeFrameDriver(player: player) {
      guard frame == nil, let copied = driver?.copyFrame() else { return }
      frame = copied
      pulled.fulfill()
    }
    driver?.start()
    wait(for: [pulled], timeout: 10)
    driver?.stop()
    XCTAssertEqual(frame.map(CVPixelBufferGetPixelFormatType),
                   kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
    let transfer = frame.flatMap {
      CVBufferCopyAttachment($0, kCVImageBufferTransferFunctionKey, nil)
    }
    XCTAssertEqual(transfer as? String, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ as String)
  }
}
