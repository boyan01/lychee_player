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

static id<FlutterTextureRegistry> textures_registry;

@interface FlutterTextureImpl : NSObject<FlutterTexture>

@property CVPixelBufferRef copyPixelBuffer;

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

@end

class MacosFlutterTexture: public FlutterMediaTexture {
public:

	MacosFlutterTexture(int64_t texture_id,
	                    FlutterTextureImpl* texture)
		: texture_id_(texture_id),
		texture_(texture),
		cv_pixel_buffer_ref_(nullptr){
	}

	~MacosFlutterTexture() override = default;

	int64_t GetTextureId() override
	{
		return texture_id_;
	}

	int GetWidth() override
	{
		return (int)CVPixelBufferGetWidth(cv_pixel_buffer_ref_);
	}

	int GetHeight() override
	{
		return (int)CVPixelBufferGetHeight(cv_pixel_buffer_ref_);
	}

	PixelFormat GetSupportFormat() override {
		return kFormat_32_BGRA;
	}

	void MaybeInitPixelBuffer(int width, int height) override {
		auto ret = CVPixelBufferCreate(kCFAllocatorDefault,
		                               width,
		                               height,
		                               kCVPixelFormatType_32BGRA,
		                               nullptr,
		                               &cv_pixel_buffer_ref_);
		if (ret != kCVReturnSuccess) {
			NSLog(@"create pixle buffer failed. error : %d", ret);
		} else {

		}
	}

	void LockBuffer() override {
		if (cv_pixel_buffer_ref_) {
			CVPixelBufferLockBaseAddress(cv_pixel_buffer_ref_, 0);
		}
	}

	void UnlockBuffer() override {
		if (cv_pixel_buffer_ref_) {
			CVPixelBufferUnlockBaseAddress(cv_pixel_buffer_ref_, 0);
		}
	}

	void NotifyBufferUpdate() override {
		[texture_ setCopyPixelBuffer: cv_pixel_buffer_ref_];
		[textures_registry textureFrameAvailable: texture_id_];
	}

	uint8_t * GetBuffer() override {
		if (cv_pixel_buffer_ref_) {
			return (uint8_t *)CVPixelBufferGetBaseAddress(cv_pixel_buffer_ref_);
		}
		return nullptr;
	}

	void Release() override
	{
		[textures_registry unregisterTexture: texture_id_];
	}

private:

	CVPixelBufferRef cv_pixel_buffer_ref_;

	int64_t texture_id_;

	FlutterTextureImpl *texture_;

};

std::unique_ptr<FlutterMediaTexture> texture_factory()
{
	FlutterTextureImpl *texture = [[FlutterTextureImpl alloc] init];

	int64_t texture_id = [textures_registry registerTexture: texture];
	return std::make_unique<MacosFlutterTexture>(texture_id, texture);
}

void reigsterMedaiFramework(id<FlutterTextureRegistry> textures)
{
	textures_registry = textures;
	register_flutter_texture_factory(texture_factory);
}
