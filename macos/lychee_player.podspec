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
  s.source_files     = 'include/*.h', 'Classes/**/*'
  s.public_header_files = 'Classes/**/*.h'
  s.dependency 'FlutterMacOS'
  s.osx.dependency 'ffmpeg-kit-macos-full', '~> 4.4.LTS'
  s.osx.vendored_libraries = 'lib/libmedia_flutter.a', 'lib/libmedia_base.a', 'lib/libmedia_macos.a', 'lib/libmedia_player.a', 'lib/libmedia_flutter_base.a'


  s.platform = :osx, '10.11'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.static_framework = true
end
