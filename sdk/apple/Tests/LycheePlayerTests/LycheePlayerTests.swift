import CoreVideo
import LycheePlayer
import XCTest

// Needs the media tool/regression.sh generates in /tmp/lychee-media.
final class LycheePlayerTests: XCTestCase {
  private let url = "/tmp/lychee-media/h264_aac.mp4"

  override func setUpWithError() throws {
    try XCTSkipUnless(FileManager.default.fileExists(atPath: url), "run tool/regression.sh first")
  }

  func testPlaysThroughFrameDriver() throws {
    let player = LycheePlayer()
    player.volume = 0
    let ready = expectation(description: "ready")
    player.onEvent = { event in
      if case .stateChanged(.ready) = event { ready.fulfill() }
    }
    player.open(url)
    player.play()
    wait(for: [ready], timeout: 10)
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

  func testSubtitlePictures() {
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
