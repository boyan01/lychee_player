#import "LycheePlayerPlugin.h"

#import "Dummy.h"

@implementation LycheePlayerPlugin
+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
  
}

- (void)handleMethodCall:(FlutterMethodCall*)call result:(FlutterResult)result {
	result(FlutterMethodNotImplemented);
  lychee_player_dummpy();
}

@end
