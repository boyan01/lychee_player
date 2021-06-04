Pod::Spec.new do |s|
    s.name         = "LycheePlayer"
    s.version      = "0.0.1"
    s.summary      = "LycheePlayer"
    s.description  = "LycheePlayer"
    s.homepage     = "https://github.com/boyan01/media_player"
    # s.license	   = { :type => 'MIT', :file => 'LICENSE.txt' }
    s.source       = { :path => 'Library' }
    s.source_files = 'Classes/**/*','include/*.h'
    s.ios.dependency 'Flutter'
    s.osx.dependency 'FlutterMacOS'
    
    # we use ffmpeg-kit to provide all ffmpeg frameworks.
    # https://github.com/tanersener/ffmpeg-kit/tree/main/apple
    s.ios.dependency 'ffmpeg-kit-ios-full', '~> 4.4.LTS'
    s.osx.dependency 'ffmpeg-kit-macos-full', '~> 4.4.LTS'
  
    s.authors       =  { 'Bin Yang' => 'yangbinyhbn@gmail.com'}
                     
    s.requires_arc = true
    s.cocoapods_version = '>= 1.9'
    s.osx.vendored_libraries = 'lib_osx/libmedia_flutter.a', 'lib_osx/libmedia_base.a', 'lib_osx/libmedia_macos.a', 'lib_osx/libmedia_player.a', 'lib_osx/libmedia_flutter_base.a'
    s.ios.vendored_libraries = 'lib_ios/libmedia_flutter.a', 'lib_ios/libmedia_base.a', 'lib_ios/libmedia_macos.a', 'lib_ios/libmedia_player.a', 'lib_ios/libmedia_flutter_base.a'
    s.ios.deployment_target = '10.0'
    s.osx.deployment_target = '10.11'
    s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'VALID_ARCHS[sdk=iphonesimulator*]' => 'x86_64' }
    s.static_framework = true
  
  end
