//
//  IOSVideoRendererSink.hpp
//  LycheePlayer
//
//  Created by Bin Yang on 2021/5/30.
//

#ifndef DarwinVideoRendererSink_hpp
#define DarwinVideoRendererSink_hpp

#include <stdio.h>

#if TARGET_OS_IPHONE
#include <Flutter/Flutter.h>
#else
#import <FlutterMacOS/FlutterMacOS.h>
#endif

__BEGIN_DECLS

void reigsterMediaFramework(id<FlutterTextureRegistry> textures);

// Add this to void static library didn't pack media_flutter.a to app bundle.
void avoidMediaFlutterShrink(void);

__END_DECLS

#endif /* DarwinVideoRendererSink_hpp */
