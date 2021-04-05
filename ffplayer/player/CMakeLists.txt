cmake_minimum_required(VERSION 3.10)

project("media_player")

set(MEDIA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/")

aux_source_directory(${MEDIA_SRC_DIR} SRC_FILES)

add_library(media_player STATIC ${SRC_FILES}
        renderer.h
        renderer.cc
        renderer_video.h
        renderer_video.cc
        ffmpeg_demuxer.h
        ffmpeg_demuxer.cc
        data_source.h
        data_source.cc
        buffer.cc buffer.h
        decoder_buffer.cc
        decoder_buffer.h
        audio_decoder_config.cc
        audio_decoder_config.h
        ffmpeg_common.h
        ffmpeg_common.cc
        channel_layout.cc
        channel_layout.h
        media_limits.h
        video_decoder_config.cc
        video_decoder_config.h
        video_frame.cc
        video_frame.h
        pipeline_status.cc
        pipeline_status.h
        ffmpeg_glue.cc
        ffmpeg_glue.h
        ranges.cpp
        ranges.h
        )

target_include_directories(media_player PUBLIC
        ${MEDIA_SRC_DIR}
        ../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}
        )

target_link_libraries(media_player PUBLIC ${FFP_LIBS} media_base)

if (NOT DISABLE_MEDIA_TEST)
    add_executable(media_player_test
            ffmpeg_common_test.cc
            ffmpeg_glue_test.cc
            ffmpeg_demuxer_test.cc
            )
    target_link_libraries(media_player_test media_player gtest_main gmock_main)

    add_test(NAME media_player_test COMMAND media_player)
endif ()
