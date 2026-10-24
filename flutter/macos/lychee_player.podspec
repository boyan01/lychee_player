# The core C++ library is compiled from ../../core through the forwarders in
# Classes/core (see tool/generate_forwarders.py). Flutter reaches this pod
# through a symlink and Xcode normalizes "../.." lexically, so the header
# paths use the resolved location of the repository's core/.
#
# FFmpeg is found with pkg-config. This is for development: release builds
# should bundle an LGPL-only FFmpeg build instead of linking the system one.

core_dir = File.expand_path('../../core', File.realpath(__dir__))

ffmpeg_modules = %w[libavformat libavcodec libavutil libswresample libswscale]
pkg_config = ENV['PKG_CONFIG'] ||
             %w[/opt/homebrew/bin/pkg-config /usr/local/bin/pkg-config].find { |p| File.executable?(p) } ||
             'pkg-config'
ffmpeg_cflags = `#{pkg_config} --cflags #{ffmpeg_modules.join(' ')}`.strip
ffmpeg_libs = `#{pkg_config} --libs #{ffmpeg_modules.join(' ')}`.strip
if ffmpeg_libs.empty?
  raise 'lychee_player: FFmpeg not found with pkg-config (brew install ffmpeg pkg-config)'
end

Pod::Spec.new do |s|
  s.name             = 'lychee_player'
  s.version          = '1.0.0'
  s.summary          = 'A cross-platform media player for Flutter.'
  s.homepage         = 'https://github.com/boyan01/lychee_player'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'boyan01' => 'https://github.com/boyan01' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*.{h,m,cc}'
  s.public_header_files = 'Classes/LycheePlayerPlugin.h'
  s.dependency 'FlutterMacOS'
  s.platform = :osx, '10.15'
  s.frameworks = 'AudioToolbox', 'CoreAudio', 'CoreMedia', 'CoreVideo',
                 'IOSurface', 'VideoToolbox'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'HEADER_SEARCH_PATHS' => "\"#{core_dir}/include\" \"#{core_dir}/src\"",
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) LYCHEE_WITH_FFMPEG=1 LYCHEE_IMPLEMENTATION=1',
    'OTHER_CFLAGS' => "$(inherited) #{ffmpeg_cflags} -Wno-documentation",
    'OTHER_LDFLAGS' => "$(inherited) #{ffmpeg_libs}",
  }
end
