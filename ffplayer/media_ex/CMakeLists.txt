cmake_minimum_required(VERSION 3.10)

project("media_ex")

add_library(media_ex STATIC
        base/time_delta.cc
        base/time_delta.h
        base/time_ticks.cc
        base/time_ticks.h
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
        decoder/decoder_stream.cc
        decoder/decoder_stream.h
        decoder/decoder_stream_traits.cc
        decoder/decoder_stream_traits.h
        decoder/video_decoder.cc
        decoder/video_decoder.h
        decoder/video_frame.h
        decoder/video_frame.cc
        decoder/decode_status.cc
        decoder/decode_status.h
        ffmpeg/ffmpeg_utils.cc
        ffmpeg/ffmpeg_utils.h
        ffmpeg/ffmpeg_glue.h
        ffmpeg/ffmpeg_glue.cc
        ffmpeg/blocking_url_protocol.h
        ffmpeg/blocking_url_protocol.cc
        ffmpeg/ffmpeg_deleters.h
        ffmpeg/ffmpeg_decoding_loop.cc
        ffmpeg/ffmpeg_decoding_loop.h
        source/data_source.cc
        source/data_source.h
        source/file_data_source.cc
        source/file_data_source.h
        pipeline/player_pipeline.cc
        pipeline/player_pipeline.h
        pipeline/pipeline_status.h
        pipeline/pipeline_status.cc)

target_include_directories(media_ex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_ex PUBLIC ${FFP_LIBS} media_base)

if (NOT DISABLE_MEDIA_TEST)
    add_executable(media_ex_test
            ffmpeg/ffmpeg_glue_unittest.cc
            ffmpeg/blocking_url_protocol_unittest.cc
            pipeline/player_pipeline_test.cc
            )
    target_link_libraries(media_ex_test media_ex gtest_main gmock_main)

    add_test(NAME media_ex_test COMMAND media_player)
endif ()