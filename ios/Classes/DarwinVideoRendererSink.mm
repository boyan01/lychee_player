//
//  IOSVideoRendererSink.cpp
//  LycheePlayer
//
//  Created by Bin Yang on 2021/5/30.
//

#import "DarwinVideoRendererSink.h"

#include <Foundation/Foundation.h>
#include <iostream>

#include "media_api.h"


#import <Foundation/Foundation.h>


static id<FlutterTextureRegistry> textures_registry;

@interface FlutterTextureImpl : NSObject<FlutterTexture>

@property(nonatomic) CVPixelBufferRef pixelBuffer;

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

- (CVPixelBufferRef)copyPixelBuffer {
	CVPixelBufferRef buffer;

	@synchronized (self) {
		buffer = _pixelBuffer;
		_pixelBuffer = nil;
	}

	return buffer;
}

- (void)setPixelBuffer:(CVPixelBufferRef)pixelBuffer {
  
  @synchronized (self) {
    if (_pixelBuffer != nil) {
      CVPixelBufferRelease(_pixelBuffer);
    }
    _pixelBuffer = pixelBuffer;
    CVPixelBufferRetain(_pixelBuffer);
  }

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
			cv_pixel_buffer_ref_ = nullptr;
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
		if (!cv_pixel_buffer_ref_) {
			return;
		}
		[texture_ setPixelBuffer: cv_pixel_buffer_ref_];
		CVPixelBufferRelease(cv_pixel_buffer_ref_);
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

void texture_factory(std::function<void(std::unique_ptr<FlutterMediaTexture>)> callback)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		FlutterTextureImpl *texture = [[FlutterTextureImpl alloc] init];

		int64_t texture_id = [textures_registry registerTexture: texture];

#if 0
		CVPixelBufferRef pixel_buffer;
		CVPixelBufferCreate(kCFAllocatorDefault,
		                    1280,
		                    720,
		                    kCVPixelFormatType_32BGRA,
		                    nullptr,
		                    &pixel_buffer);
		CVPixelBufferLockBaseAddress(pixel_buffer, 0);
		auto buffer = (uint8_t *)CVPixelBufferGetBaseAddress(pixel_buffer);
		for ( unsigned long i = 0; i < 1280 * 720; i++ )
		{
			buffer[i] = CFSwapInt32HostToBig(0x000000ff);
		}
		CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
		texture.copyPixelBuffer = pixel_buffer;
#endif

		NSLog(@"register texture: %lld.", texture_id);
		callback(std::make_unique<MacosFlutterTexture>(texture_id, texture));

	});

}

void reigsterMediaFramework(id<FlutterTextureRegistry> textures)
{
	if (!textures) {
		NSLog(@"null textures");
	}
	textures_registry = textures;
	register_flutter_texture_factory(texture_factory);
}
