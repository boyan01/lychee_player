# One pod for iOS and macOS (sharedDarwinSource in pubspec.yaml).
#
# The core, FFmpeg and libass come prebuilt, as the xcframeworks in
# Frameworks/: lychee.xcframework (tool/build_apple.sh), the LGPL-only
# FFmpeg ones (tool/build_ffmpeg.sh) and libass with FriBidi
# (tool/build_libass.sh), both packaged by tool/package_ffmpeg_apple.sh.
# The plugin uses only the C ABI in lychee/lychee.h; a pod cannot depend on
# the Swift package in sdk/apple, so it keeps its own copy of its frame
# driver (lychee_player/Package.swift uses the SDK's).
#
# Frameworks built locally win, when all of them are there. Otherwise they
# come from the release that ../prebuilt.json pins (tool/release.sh): the
# xcframeworks it lists (1.0.0-beta.1 had no libass), downloaded once into
# ~/.cache/lychee_player and checked against its SHA-256s;
# LYCHEE_PREBUILT_URL replaces the release URL with a mirror's.

require 'digest'
require 'fileutils'
require 'json'

frameworks = %w[lychee libavformat libavcodec libswresample libswscale libavutil libass libfribidi]

lambda do
  dir = File.join(__dir__, 'Frameworks')
  manifest = JSON.parse(File.read(File.join(__dir__, '..', 'prebuilt.json')))
  tag = manifest['tag'] or return
  zips = manifest['files'].select { |name, _| name.end_with?('.xcframework.zip') }.to_a
  stamp = File.join(dir, '.prebuilt')
  want = ([tag] + zips.map { |z| z.join(' ') }).join("\n")
  if File.exist?(stamp)
    return if File.read(stamp) == want
  elsif frameworks.all? { |f| Dir.exist?(File.join(dir, "#{f}.xcframework")) }
    return
  end

  url = ENV['LYCHEE_PREBUILT_URL'].to_s.empty? ? manifest['url'] : ENV['LYCHEE_PREBUILT_URL']
  cache_home = ENV['XDG_CACHE_HOME'].to_s.empty? ? File.join(Dir.home, '.cache') : ENV['XDG_CACHE_HOME']
  cache = File.join(cache_home, 'lychee_player', tag)
  FileUtils.mkdir_p(cache)
  zips.each do |name, sha|
    zip = File.join(cache, name)
    next if File.exist?(zip) && Digest::SHA256.file(zip).hexdigest == sha
    from = "#{url}/#{tag}/#{name}"
    Pod::UI.puts "lychee_player: downloading #{from}"
    system('curl', '-fsSL', '--retry', '3', '-o', "#{zip}.part", from) or
      raise "lychee_player: cannot download #{from}; LYCHEE_PREBUILT_URL can name a mirror"
    got = Digest::SHA256.file("#{zip}.part").hexdigest
    raise "lychee_player: #{from} has SHA-256 #{got}, not #{sha}" unless got == sha
    File.rename("#{zip}.part", zip)
  end
  FileUtils.rm_rf(dir)
  FileUtils.mkdir_p(dir)
  zips.each do |name, _|
    system('unzip', '-q', File.join(cache, name), '-d', dir) or raise "lychee_player: cannot unzip #{name}"
  end
  File.write(stamp, want)
end.call

Pod::Spec.new do |s|
  s.name             = 'lychee_player'
  s.version          = '1.0.0'
  s.summary          = 'A cross-platform media player for Flutter.'
  s.homepage         = 'https://github.com/boyan01/lychee_player'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'boyan01' => 'https://github.com/boyan01' }
  s.source           = { :path => '.' }
  # The layout Flutter's Swift Package Manager integration wants
  # (lychee_player/Package.swift builds the same sources).
  s.source_files     = 'lychee_player/Sources/lychee_player/**/*.{h,m}'
  s.public_header_files = 'lychee_player/Sources/lychee_player/include/**/*.h'
  s.ios.dependency 'Flutter'
  s.osx.dependency 'FlutterMacOS'
  # The same as in tool/apple_framework.sh.
  s.ios.deployment_target = '16.0'
  s.osx.deployment_target = '11.0'
  s.frameworks = 'CoreVideo', 'QuartzCore'
  s.ios.frameworks = 'AVFAudio'

  present = frameworks.select { |f| Dir.exist?(File.join(__dir__, 'Frameworks', "#{f}.xcframework")) }
  # A release's are complete with the ones it has.
  unless File.exist?(File.join(__dir__, 'Frameworks', '.prebuilt')) || present == frameworks
    Pod::UI.warn "lychee_player: missing #{(frameworks - present).join(', ')}.xcframework in Frameworks/; " \
                 'build them with tool/build_ffmpeg.sh, tool/build_libass.sh, tool/package_ffmpeg_apple.sh ' \
                 'and tool/build_apple.sh'
  end
  s.vendored_frameworks = present.map { |f| "Frameworks/#{f}.xcframework" }

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
