#import "LycheePlayerPlugin.h"

#import <DarwinVideoRendererSink.h>

@implementation LycheePlayerPlugin
+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
	reigsterMediaFramework(registrar.textures);
}

- (void)handleMethodCall:(FlutterMethodCall*)call result:(FlutterResult)result {
	result(FlutterMethodNotImplemented);
}

@end
