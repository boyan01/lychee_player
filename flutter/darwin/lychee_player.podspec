# One pod for iOS and macOS (sharedDarwinSource in pubspec.yaml).
#
# The core and FFmpeg come prebuilt, as the xcframeworks in Frameworks/:
# lychee.xcframework (tool/build_apple.sh) and the LGPL-only FFmpeg ones
# (tool/build_ffmpeg.sh, packaged by tool/package_ffmpeg_apple.sh).
# The plugin uses only the C ABI in lychee/lychee.h; a pod cannot depend on
# the Swift package in sdk/apple, so it keeps its own copy of its frame
# driver (lychee_player/Package.swift uses the SDK's).
#
# Frameworks built locally win, when all of them are there. Otherwise they
# come from the release that ../prebuilt.json pins (tool/release.sh): the
# xcframeworks it lists, downloaded once into
# ~/.cache/lychee_player and checked against its SHA-256s;
# LYCHEE_PREBUILT_URL replaces the release URL with a mirror's.

require 'digest'
require 'fileutils'
require 'json'

backend = ENV.fetch('LYCHEE_WITH_FFMPEG', 'OFF').upcase
raise 'LYCHEE_WITH_FFMPEG must be ON or OFF' unless %w[ON OFF].include?(backend)
with_ffmpeg = backend == 'ON'
artifacts = with_ffmpeg ? %w[lychee libavformat libavcodec libswresample libswscale libavutil] : %w[lychee_noffmpeg]
frameworks = with_ffmpeg ? artifacts : %w[lychee]
root = File.join(__dir__, 'Frameworks')
local = !File.exist?(File.join(root, '.prebuilt')) &&
        artifacts.all? { |name| Dir.exist?(File.join(root, "#{name}.xcframework")) }

if local
  framework_dir = root
  unless with_ffmpeg
    # CocoaPods takes -framework from the outer basename. The base artifact
    # also contains lychee.framework; alias it without replacing the full core.
    framework_dir = File.join(root, 'noffmpeg')
    FileUtils.mkdir_p(framework_dir)
    link = File.join(framework_dir, 'lychee.xcframework')
    FileUtils.ln_sf('../lychee_noffmpeg.xcframework', link) unless File.exist?(link)
  end
else
  manifest = JSON.parse(File.read(File.join(__dir__, '..', 'prebuilt.json')))
  tag = manifest.fetch('tag')
  zips = artifacts.map { |name| ["#{name}.xcframework.zip", manifest.fetch('files').fetch("#{name}.xcframework.zip")] }
  identity = Digest::SHA256.hexdigest(zips.flatten.join("\n"))[0, 16]
  # Apps share pub's package directory. Different versions/backends coexist;
  # selecting one must never delete frameworks another app is building with.
  framework_dir = File.join(root, 'prebuilt', "#{tag}-#{identity}")
  url = ENV['LYCHEE_PREBUILT_URL'].to_s.empty? ? manifest.fetch('url') : ENV['LYCHEE_PREBUILT_URL']
  cache_home = ENV['XDG_CACHE_HOME'].to_s.empty? ? File.join(Dir.home, '.cache') : ENV['XDG_CACHE_HOME']
  cache = File.join(cache_home, 'lychee_player', tag)
  FileUtils.mkdir_p(cache)
  File.open(File.join(cache, "#{identity}.lock"), 'w') do |lock|
    lock.flock(File::LOCK_EX)
    unless File.exist?(File.join(framework_dir, '.complete'))
      FileUtils.mkdir_p(framework_dir)
      zips.each do |name, sha|
        zip = File.join(cache, "#{sha}-#{name}")
        unless File.exist?(zip) && Digest::SHA256.file(zip).hexdigest == sha
          from = "#{url}/#{tag}/#{name}"
          Pod::UI.puts "lychee_player: downloading #{from}"
          system('curl', '-fsSL', '--retry', '3', '-o', "#{zip}.part", from) or
            raise "lychee_player: cannot download #{from}; LYCHEE_PREBUILT_URL can name a mirror"
          raise "lychee_player: SHA-256 mismatch for #{name}" unless Digest::SHA256.file("#{zip}.part").hexdigest == sha
          File.rename("#{zip}.part", zip)
        end
        system('unzip', '-oq', zip, '-d', framework_dir) or raise "lychee_player: cannot unzip #{name}"
      end
      unless with_ffmpeg
        target = File.join(framework_dir, 'lychee.xcframework')
        File.rename(File.join(framework_dir, 'lychee_noffmpeg.xcframework'), target) unless Dir.exist?(target)
      end
      File.write(File.join(framework_dir, '.complete'), '')
    end
  end
end
framework_path = framework_dir.delete_prefix("#{__dir__}/")

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

  s.vendored_frameworks = frameworks.map { |name| "#{framework_path}/#{name}.xcframework" }

  # CocoaPods copies the xcframework slices too late for this pod's own
  # compile and link, so point at the slices directly.
  slice_paths = lambda do |slice|
    "$(inherited) \"${PODS_TARGET_SRCROOT}/#{framework_path}/lychee.xcframework/#{slice}\""
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
