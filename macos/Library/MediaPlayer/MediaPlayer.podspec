Pod::Spec.new do |s|
    s.name         = "MediaPlayer"
    s.version      = "0.0.1"
    s.summary      = "MediaPlayer"
    s.description  = "MediaPlayer"
    s.homepage     = "https://github.com/boyan01/media_player"
    # s.license	   = { :type => 'MIT', :file => 'LICENSE.txt' }
    s.source       = { :path => 'Library' }
    s.source_files = 'Classes/**/*'
    s.dependency 'FlutterMacOS'
  
    s.authors       =  { 'Bin Yang' => 'yangbinyhbn@gmail.com'}
                     
    s.requires_arc = true
    s.cocoapods_version = '>= 1.9'
    s.pod_target_xcconfig = { "HEADER_SEARCH_PATHS" => $dir}
    s.vendored_library = 'Frameworks/lib/*.dylib'
#    s.private_header_files = 'Frameworks/include/*.h'
    s.macos.public_header_files = 'Frameworks/**/*.{h}'

  end
