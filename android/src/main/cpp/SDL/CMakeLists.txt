project("SDL" "C")

include(AndroidNdkModules)
android_ndk_import_module_cpufeatures()


add_compile_definitions(GL_GLEXT_PROTOTYPES)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wdocumentation -Wdocumentation-unknown-command -Wmissing-prototypes -Wunreachable-code-break -Wunneeded-internal-declaration -Wmissing-variable-declarations -Wfloat-conversion -Wshorten-64-to-32 -Wunreachable-code-return -Wshift-sign-overflow -Wstrict-prototypes -Wkeyword-macro")

set(SDL_SOURCE_DIR ../../../../../thirdparty/SDL/src)
aux_source_directory(${SDL_SOURCE_DIR} SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/audio SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/audio/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/audio/dummy SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/audio/openslES SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/core/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/cpuinfo SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/dynapi SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/events SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/file SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/haptic SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/haptic/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/joystick SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/joystick/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/joystick/hidapi SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/joystick/virtual SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/loadso/dlopen SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/locale SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/locale/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/misc SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/misc/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/power SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/power/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/filesystem/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/sensor SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/sensor/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/software SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/direct3d SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/direct3d11 SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/metal SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/opengl SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/opengles SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/opengles2 SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/render/psp SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/stdlib SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/thread SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/thread/pthread SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/timer SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/timer/unix SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/video SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/video/android SRC_FILES)
aux_source_directory(${SDL_SOURCE_DIR}/video/yuv2rgb SRC_FILES)

add_library(SDL2 SHARED
        ${SRC_FILES}
        ${SDL_SOURCE_DIR}/atomic/SDL_atomic.c
        ${SDL_SOURCE_DIR}/atomic/SDL_spinlock.c
        )

target_include_directories(SDL2 PUBLIC ../../../../../thirdparty/SDL/include)


add_library(hidapi ${SDL_SOURCE_DIR}/hidapi/android/hid.cpp)
target_link_libraries(hidapi log)
target_link_libraries(SDL2 cpufeatures log dl GLESv1_CM GLESv2 OpenSLES android hidapi)
