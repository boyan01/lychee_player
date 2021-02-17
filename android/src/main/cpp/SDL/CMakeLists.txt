project("SDL" "C")

include(AndroidNdkModules)
android_ndk_import_module_cpufeatures()


add_compile_definitions(GL_GLEXT_PROTOTYPES)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wdocumentation -Wdocumentation-unknown-command -Wmissing-prototypes -Wunreachable-code-break -Wunneeded-internal-declaration -Wmissing-variable-declarations -Wfloat-conversion -Wshorten-64-to-32 -Wunreachable-code-return -Wshift-sign-overflow -Wstrict-prototypes -Wkeyword-macro")

aux_source_directory(src SRC_FILES)
aux_source_directory(src/audio SRC_FILES)
aux_source_directory(src/audio/android SRC_FILES)
aux_source_directory(src/audio/dummy SRC_FILES)
aux_source_directory(src/audio/openslES SRC_FILES)
aux_source_directory(src/core/android SRC_FILES)
aux_source_directory(src/cpuinfo SRC_FILES)
aux_source_directory(src/dynapi SRC_FILES)
aux_source_directory(src/events SRC_FILES)
aux_source_directory(src/file SRC_FILES)
aux_source_directory(src/haptic SRC_FILES)
aux_source_directory(src/haptic/android SRC_FILES)
aux_source_directory(src/joystick SRC_FILES)
aux_source_directory(src/joystick/android SRC_FILES)
aux_source_directory(src/joystick/hidapi SRC_FILES)
aux_source_directory(src/joystick/virtual SRC_FILES)
aux_source_directory(src/loadso/dlopen SRC_FILES)
aux_source_directory(src/locale SRC_FILES)
aux_source_directory(src/locale/android SRC_FILES)
aux_source_directory(src/misc SRC_FILES)
aux_source_directory(src/misc/android SRC_FILES)
aux_source_directory(src/power SRC_FILES)
aux_source_directory(src/power/android SRC_FILES)
aux_source_directory(src/filesystem/android SRC_FILES)
aux_source_directory(src/sensor SRC_FILES)
aux_source_directory(src/sensor/android SRC_FILES)
aux_source_directory(src/render SRC_FILES)
aux_source_directory(src/render/software SRC_FILES)
aux_source_directory(src/render/direct3d SRC_FILES)
aux_source_directory(src/render/direct3d11 SRC_FILES)
aux_source_directory(src/render/metal SRC_FILES)
aux_source_directory(src/render/opengl SRC_FILES)
aux_source_directory(src/render/opengles SRC_FILES)
aux_source_directory(src/render/opengles2 SRC_FILES)
aux_source_directory(src/render/psp SRC_FILES)
aux_source_directory(src/stdlib SRC_FILES)
aux_source_directory(src/thread SRC_FILES)
aux_source_directory(src/thread/pthread SRC_FILES)
aux_source_directory(src/timer SRC_FILES)
aux_source_directory(src/timer/unix SRC_FILES)
aux_source_directory(src/video SRC_FILES)
aux_source_directory(src/video/android SRC_FILES)
aux_source_directory(src/video/yuv2rgb SRC_FILES)

add_library(SDL2 SHARED
        ${SRC_FILES}
        src/atomic/SDL_atomic.c
        src/atomic/SDL_spinlock.c
        )

target_include_directories(SDL2 PUBLIC include)


add_library(hidapi src/hidapi/android/hid.cpp)
target_link_libraries(hidapi log)
target_link_libraries(SDL2 cpufeatures log dl GLESv1_CM GLESv2 OpenSLES android hidapi)
