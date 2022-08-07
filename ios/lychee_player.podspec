#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint lychee_player.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'lychee_player'
  s.version          = '0.0.1'
  s.summary          = 'A new flutter plugin project.'
  s.description      = <<-DESC
A new flutter plugin project.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }
  s.source           = { :path => '.' }
  s.source_files = 'include/*.h', 'Classes/**/*'
  s.public_header_files = 'Classes/**/*.h'
  s.dependency 'Flutter'
  s.platform = :ios, '10.0'
  s.ios.dependency 'ffmpeg-kit-ios-full', '~> 4.4.LTS'
  s.ios.vendored_libraries = 'lib/libmedia_flutter.a', 'lib/libmedia_base.a', 'lib/libmedia_macos.a', 'lib/libmedia_player.a'

  # Flutter.framework does not contain a i386 slice.
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386' }

  s.static_framework = true


end
