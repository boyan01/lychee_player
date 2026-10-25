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
}
