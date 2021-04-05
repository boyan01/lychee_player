#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint media_player.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'media_player'
  s.version          = '0.0.1'
  s.summary          = 'A new flutter plugin project.'
  s.description      = <<-DESC
A new flutter plugin project.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.12'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
  
  s.subspec 'FlutterMediaPlayer' do |sss|
    sss.platform = :osx, '10.12'
    sss.vendored_frameworks = 'ffplayer/cmake-build-debug/flutter/media_flutter.framework'
  end
end
