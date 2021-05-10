Pod::Spec.new do |s|
    s.name         = "MediaPlayer"
    s.version      = "0.0.1"
    s.summary      = "MediaPlayer"
    s.description  = "MediaPlayer"
    s.homepage     = "https://github.com/boyan01/media_player"
    s.license	   = { :type => 'MIT', :file => 'LICENSE.txt' }
    s.source       = { :path => '.' }
  
    s.authors       =  { 'Bin Yang' => 'yangbinyhbn@gmail.com'}
                     
    s.requires_arc = true
    s.cocoapods_version = '>= 1.9'
    s.ios.deployment_target = '10.0'
    s.osx.deployment_target = '10.10'
    s.vendored_frameworks = './Frameworks/libmedia_player.a'
    s.public_header_files = './Frameworks/include/*.h'
  end