cmake_minimum_required(VERSION 3.10)

project("media_player")

set(MEDIA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/")

aux_source_directory(${MEDIA_SRC_DIR} SRC_FILES)

add_library(media_player STATIC ${SRC_FILES}
        renderer/renderer.h
        renderer/renderer.cc
        renderer/audio_renderer.cc
        renderer/audio_renderer.h
        renderer/video_renderer.cc
        renderer/video_renderer.h
        demuxer/demuxer_stream.cc
        demuxer/demuxer_stream.h
        demuxer/demuxer.cc
        demuxer/demuxer.h
        decoder/audio_decoder_config.cc
        decoder/audio_decoder_config.h
        decoder/video_decoder_config.cc
        decoder/video_decoder_config.h
        decoder/decoder_buffer.h
        decoder/decoder_buffer.cc
        decoder/media_track.h
        decoder/media_track.cc
        decoder/media_tracks.h
        decoder/media_tracks.cc
        ffmpeg/ffmpeg_utils.cc
        ffmpeg/ffmpeg_utils.h
        ffmpeg/ffmpeg_glue.h
        ffmpeg/ffmpeg_glue.cc
        ffmpeg/blocking_url_protocol.h
        ffmpeg/blocking_url_protocol.cc
        ffmpeg/ffmpeg_deleters.h
        source/data_source.cc
        source/data_source.h
        source/file_data_source.cc
        source/file_data_source.h
        pipeline/player_pipeline.cc
        pipeline/player_pipeline.h
        pipeline/pipeline_status.h
        pipeline/pipeline_status.cc
        )

target_include_directories(media_player PUBLIC
        ${MEDIA_SRC_DIR}
        ../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}
        )

target_link_libraries(media_player PUBLIC ${FFP_LIBS} media_base)

if (NOT DISABLE_MEDIA_TEST)
    add_executable(media_player_test
            ffmpeg/ffmpeg_glue_unittest.cc
            ffmpeg/blocking_url_protocol_unittest.cc
            pipeline/player_pipeline_test.cc
            )
    target_link_libraries(media_player_test media_player gtest_main gmock_main)

    add_test(NAME media_player_test COMMAND media_player)
endif ()
