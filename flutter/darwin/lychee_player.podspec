# One pod for iOS and macOS (sharedDarwinSource in pubspec.yaml).
#
# The core C++ library is compiled from ../../core through the forwarders in
# Classes/core (see tool/generate_forwarders.py). Flutter reaches this pod
# through a symlink and Xcode normalizes "../.." lexically, so the header
# paths use the resolved location of the repository's core/.
#
# FFmpeg:
# - iOS: the LGPL-only xcframeworks in Frameworks/, built with
#   tool/build_ffmpeg.sh and tool/package_ffmpeg_apple.sh.
# - macOS: found with pkg-config. This is for development: release builds
#   should bundle an LGPL-only FFmpeg build instead of linking the system one.

core_dir = File.expand_path('../../core', File.realpath(__dir__))

ffmpeg_modules = %w[libavformat libavcodec libavutil libswresample libswscale]
pkg_config = ENV['PKG_CONFIG'] ||
             %w[/opt/homebrew/bin/pkg-config /usr/local/bin/pkg-config].find { |p| File.executable?(p) } ||
             'pkg-config'
ffmpeg_cflags = `#{pkg_config} --cflags #{ffmpeg_modules.join(' ')} 2>/dev/null`.strip
ffmpeg_libs = `#{pkg_config} --libs #{ffmpeg_modules.join(' ')} 2>/dev/null`.strip

Pod::Spec.new do |s|
  s.name             = 'lychee_player'
  s.version          = '1.0.0'
  s.summary          = 'A cross-platform media player for Flutter.'
  s.homepage         = 'https://github.com/boyan01/lychee_player'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'boyan01' => 'https://github.com/boyan01' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*.{h,m,cc,mm}'
  s.public_header_files = 'Classes/LycheePlayerPlugin.h'
  s.ios.dependency 'Flutter'
  s.osx.dependency 'FlutterMacOS'
  s.ios.deployment_target = '13.0'
  s.osx.deployment_target = '10.15'
  s.frameworks = 'AudioToolbox', 'CoreAudio', 'CoreMedia', 'CoreVideo',
                 'IOSurface', 'VideoToolbox'
  s.ios.frameworks = 'AVFAudio'

  common = {
    'DEFINES_MODULE' => 'YES',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'HEADER_SEARCH_PATHS' => "\"#{core_dir}/include\" \"#{core_dir}/src\"",
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) LYCHEE_WITH_FFMPEG=1 LYCHEE_IMPLEMENTATION=1',
  }

  unless Dir.exist?(File.join(__dir__, 'Frameworks', 'libavcodec.xcframework'))
    Pod::UI.warn 'lychee_player: no FFmpeg xcframeworks; iOS builds need them (tool/build_ffmpeg.sh, tool/package_ffmpeg_apple.sh)'
  end
  s.ios.vendored_frameworks = 'Frameworks/*.xcframework'
  # CocoaPods copies the xcframework slices too late for this pod's own
  # compile and link, so point at the slices directly.
  ffmpeg_frameworks = %w[libavformat libavcodec libswresample libswscale libavutil]
  slice_paths = lambda do |slice|
    ffmpeg_frameworks.map { |f| "\"${PODS_TARGET_SRCROOT}/Frameworks/#{f}.xcframework/#{slice}\"" }.join(' ')
  end
  s.ios.pod_target_xcconfig = common.merge(
    'HEADER_SEARCH_PATHS' => common['HEADER_SEARCH_PATHS'] + ' "${PODS_TARGET_SRCROOT}/Frameworks/include"',
    'FRAMEWORK_SEARCH_PATHS[sdk=iphonesimulator*]' => "$(inherited) #{slice_paths.call('ios-arm64_x86_64-simulator')}",
    'FRAMEWORK_SEARCH_PATHS[sdk=iphoneos*]' => "$(inherited) #{slice_paths.call('ios-arm64')}",
    'OTHER_LDFLAGS' => '$(inherited) ' + ffmpeg_frameworks.map { |f| "-framework #{f}" }.join(' '),
    'OTHER_CFLAGS' => '$(inherited) -Wno-documentation',
  )

  if ffmpeg_libs.empty?
    Pod::UI.warn 'lychee_player: FFmpeg not found with pkg-config; macOS builds need it (brew install ffmpeg pkg-config)'
  end
  s.osx.pod_target_xcconfig = common.merge(
    'OTHER_CFLAGS' => "$(inherited) #{ffmpeg_cflags} -Wno-documentation",
    'OTHER_LDFLAGS' => "$(inherited) #{ffmpeg_libs}",
  )
end
