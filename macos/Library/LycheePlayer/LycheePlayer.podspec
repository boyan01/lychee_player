Pod::Spec.new do |s|
    s.name         = "LycheePlayer"
    s.version      = "0.0.1"
    s.summary      = "LycheePlayer"
    s.description  = "LycheePlayer"
    s.homepage     = "https://github.com/boyan01/media_player"
    # s.license	   = { :type => 'MIT', :file => 'LICENSE.txt' }
    s.source       = { :path => 'Library' }
    s.source_files = 'Classes/**/*'
    s.dependency 'FlutterMacOS'
  
    s.authors       =  { 'Bin Yang' => 'yangbinyhbn@gmail.com'}
                     
    s.requires_arc = true
    s.cocoapods_version = '>= 1.9'
    # s.pod_target_xcconfig = { "HEADER_SEARCH_PATHS" => "$(SRCROOT)/Library/MediaPlayer/Frameworks/include"}
    s.vendored_framework = 'frameworks/media_flutter.framework'
    s.libraries = "c++"
    s.frameworks = 'media_flutter'

  end
