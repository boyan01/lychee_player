//
//  MacosVideoRendererSink.m
//  LycheePlayer
//
//  Created by 杨彬 on 2021/5/15.
//

#import "MacosVideoRendererSink.h"

#import <Foundation/Foundation.h>
#import <FlutterMacOS/FlutterTexture.h>

#include "media_flutter/media_api.h"

@interface FlutterTextureImpl : NSObject<FlutterTexture>

@end

@implementation FlutterTextureImpl

- (instancetype)init
{
    self = [super init];
    if (self) {
        NSLog(@"init FlutterTextureImpl obj");
    }
    return self;
}

- (CVPixelBufferRef _Nullable)copyPixelBuffer {
    return nullptr;
}

@end

class MacosFlutterTexture: public FlutterMediaTexture {
public:

    MacosFlutterTexture(int64_t texture_id) : texture_id_(texture_id) {
    }

    ~MacosFlutterTexture() override = default;

    int64_t GetTextureId() override
    {
        return texture_id_;
    }

    int GetWidth() override
    {
        return 0;
    }

    int GetHeight() override
    {
        return 0;
    }

    uint8_t * GetOutputBuffer() override
    {
        return 0;
    }

    void Render() override
    {
    }

    void Release() override
    {
    }

private:

    id texture_;

    int64_t texture_id_;
};

std::unique_ptr<FlutterMediaTexture> texture_factory()
{
    NSObject<FlutterTextureRegistry> *texture_registry_;
    FlutterTextureImpl *texture_ = [[FlutterTextureImpl alloc] init];

    int64_t texture_id = [texture_registry_ registerTexture:texture_];

    return std::make_unique<MacosFlutterTexture>(texture_id);
}

void reigsterMedaiFramework()
{
    NSLog(@"reigsterMediaFramework\n");
    register_flutter_texture_factory(texture_factory);
}
