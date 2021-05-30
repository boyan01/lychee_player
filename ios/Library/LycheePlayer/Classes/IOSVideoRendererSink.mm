//
//  IOSVideoRendererSink.cpp
//  LycheePlayer
//
//  Created by Bin Yang on 2021/5/30.
//

#include "IOSVideoRendererSink.h"

#include <Foundation/Foundation.h>
#include <iostream>

#include "media_api.h"

static std::unique_ptr<FlutterMediaTexture> create_flutter_texture(){
	return std::unique_ptr<FlutterMediaTexture>(nullptr);
}

void reigsterMediaFramework(id<FlutterTextureRegistry> textures){
	std::cout << "reigsterMediaFramework" << std::endl;
	register_flutter_texture_factory(create_flutter_texture);
}
