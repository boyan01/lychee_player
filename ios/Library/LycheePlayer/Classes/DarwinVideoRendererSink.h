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
#elif TAGET_OS_MACOS
#import <FlutterMacOS/FlutterTexture.h>
#endif


__BEGIN_DECLS

void reigsterMediaFramework(id<FlutterTextureRegistry> textures);

__END_DECLS

#endif /* DarwinVideoRendererSink_hpp */
