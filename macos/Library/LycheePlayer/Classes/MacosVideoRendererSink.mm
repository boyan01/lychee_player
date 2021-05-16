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

@property CVPixelBufferRef pixelBuffer;

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
//	NSLog(@" copyPixelBuffer _pixelBuffer = %d", _pixelBuffer == nullptr);
	return self->_pixelBuffer;
}

@end

class MacosFlutterTexture: public FlutterMediaTexture {
public:

	MacosFlutterTexture(int64_t texture_id,
	                    FlutterTextureImpl* texture)
		: texture_id_(texture_id),
		texture_(texture),
		cv_pixel_buffer_ref_(nullptr) {
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

	void Render(uint8_t **data, int *line_size, int width, int height) override {

		if (!data[0]) {
			NSLog(@"data[0] check failed.");
			return;
		}

		NSDictionary *options = [NSDictionary dictionaryWithObjectsAndKeys:
		                         [NSNumber numberWithBool:YES], kCVPixelBufferCGImageCompatibilityKey,
		                         //                             [NSNumber numberWithBool:YES], kCVPixelBufferCGBitmapContextCompatibilityKey,
		                         @(line_size[0]), kCVPixelBufferBytesPerRowAlignmentKey,
//		                         [NSNumber numberWithBool:YES], kCVPixelBufferOpenGLESCompatibilityKey,
		                         [NSDictionary dictionary], kCVPixelBufferIOSurfacePropertiesKey,
		                         nil];

		if (line_size[1] != line_size[2]) {
			NSLog(@"line size is invalid. %d vs %d", line_size[1], line_size[2]);
			return;
		}

		size_t srcPlaneSize = line_size[1]*height/2;
		size_t dstPlaneSize = srcPlaneSize *2;
		uint8_t *dstPlane = static_cast<uint8_t*>(malloc(dstPlaneSize));

		// interleave Cb and Cr plane
		for(size_t i = 0; i<srcPlaneSize; i++) {
			dstPlane[2*i  ]=data[1][i];
			dstPlane[2*i+1]=data[2][i];
		}


		int ret = CVPixelBufferCreate(kCFAllocatorDefault,
		                              width,
		                              height,
		                              kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
		                              (__bridge CFDictionaryRef)(options),
		                              &cv_pixel_buffer_ref_);

		CVPixelBufferLockBaseAddress(cv_pixel_buffer_ref_, 0);

		size_t bytePerRowY = CVPixelBufferGetBytesPerRowOfPlane(cv_pixel_buffer_ref_, 0);
		size_t bytesPerRowUV = CVPixelBufferGetBytesPerRowOfPlane(cv_pixel_buffer_ref_, 1);

		void* base =  CVPixelBufferGetBaseAddressOfPlane(cv_pixel_buffer_ref_, 0);
		memcpy(base, data[0], bytePerRowY*height);

		base = CVPixelBufferGetBaseAddressOfPlane(cv_pixel_buffer_ref_, 1);
		memcpy(base, dstPlane, bytesPerRowUV*height/2);


		CVPixelBufferUnlockBaseAddress(cv_pixel_buffer_ref_, 0);

		free(dstPlane);

		if (ret != kCVReturnSuccess) {
			NSLog(@"create pixle buffer failed. error : %d", ret);
			[texture_ setPixelBuffer: nullptr];
		} else {
			[texture_ setPixelBuffer: cv_pixel_buffer_ref_];
			[textures_registry textureFrameAvailable: texture_id_];
		}
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
