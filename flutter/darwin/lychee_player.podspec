# One pod for iOS and macOS (sharedDarwinSource in pubspec.yaml).
#
# The core and FFmpeg come prebuilt, as the xcframeworks in Frameworks/:
# lychee.xcframework (tool/build_apple.sh) and the LGPL-only FFmpeg ones
# (tool/build_ffmpeg.sh, tool/package_ffmpeg_apple.sh). The plugin uses only
# the C ABI in lychee/lychee.h.

frameworks = %w[lychee libavformat libavcodec libswresample libswscale libavutil]

Pod::Spec.new do |s|
  s.name             = 'lychee_player'
  s.version          = '1.0.0'
  s.summary          = 'A cross-platform media player for Flutter.'
  s.homepage         = 'https://github.com/boyan01/lychee_player'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'boyan01' => 'https://github.com/boyan01' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*.{h,m}'
  s.public_header_files = 'Classes/LycheePlayerPlugin.h'
  s.ios.dependency 'Flutter'
  s.osx.dependency 'FlutterMacOS'
  # The same as in tool/apple_framework.sh.
  s.ios.deployment_target = '16.0'
  s.osx.deployment_target = '11.0'
  s.frameworks = 'CoreVideo', 'QuartzCore'
  s.ios.frameworks = 'AVFAudio'

  missing = frameworks.reject { |f| Dir.exist?(File.join(__dir__, 'Frameworks', "#{f}.xcframework")) }
  unless missing.empty?
    Pod::UI.warn "lychee_player: missing #{missing.join(', ')}.xcframework in Frameworks/; " \
                 'build them with tool/build_ffmpeg.sh, tool/package_ffmpeg_apple.sh and tool/build_apple.sh'
  end
  s.vendored_frameworks = frameworks.map { |f| "Frameworks/#{f}.xcframework" }

  # CocoaPods copies the xcframework slices too late for this pod's own
  # compile and link, so point at the slices directly.
  slice_paths = lambda do |slice|
    "$(inherited) \"${PODS_TARGET_SRCROOT}/Frameworks/lychee.xcframework/#{slice}\""
  end
  common = {
    'DEFINES_MODULE' => 'YES',
    'OTHER_LDFLAGS' => '$(inherited) -framework lychee',
  }
  s.ios.pod_target_xcconfig = common.merge(
    'FRAMEWORK_SEARCH_PATHS[sdk=iphonesimulator*]' => slice_paths.call('ios-arm64_x86_64-simulator'),
    'FRAMEWORK_SEARCH_PATHS[sdk=iphoneos*]' => slice_paths.call('ios-arm64'),
  )
  s.osx.pod_target_xcconfig = common.merge(
    'FRAMEWORK_SEARCH_PATHS' => slice_paths.call('macos-arm64_x86_64'),
  )
end
