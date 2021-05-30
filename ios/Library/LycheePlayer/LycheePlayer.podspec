Pod::Spec.new do |s|
    s.name         = "LycheePlayer"
    s.version      = "0.0.1"
    s.summary      = "LycheePlayer"
    s.description  = "LycheePlayer"
    s.homepage     = "https://github.com/boyan01/media_player"
    # s.license	   = { :type => 'MIT', :file => 'LICENSE.txt' }
    s.source       = { :path => 'Library' }
    s.source_files = 'Classes/**/*','include/*.h'
    s.dependency 'Flutter'
    
    # we use ffmpeg-kit-ios to provide all ffmpeg frameworks.
    s.dependency 'ffmpeg-kit-ios-full', '~> 4.4.LTS'
  
    s.authors       =  { 'Bin Yang' => 'yangbinyhbn@gmail.com'}
                     
    s.requires_arc = true
    s.cocoapods_version = '>= 1.9'
    s.vendored_libraries = 'lib/libmedia_flutter.a', 'lib/libmedia_base.a', 'lib/libmedia_macos.a', 'lib/libmedia_player.a', 'lib/libmedia_flutter_base.a'
    s.platform = :ios, '10.0'
    s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'VALID_ARCHS[sdk=iphonesimulator*]' => 'x86_64' }

  end
