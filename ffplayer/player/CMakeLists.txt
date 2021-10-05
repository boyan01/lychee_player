cmake_minimum_required(VERSION 3.10)

project("media_player")

set(MEDIA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/")

aux_source_directory(${MEDIA_SRC_DIR} SRC_FILES)

add_library(media_player STATIC ${SRC_FILES})

target_include_directories(media_player PUBLIC
        ${MEDIA_SRC_DIR}
        ../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}
        )

target_link_libraries(media_player PUBLIC ${FFP_LIBS} media_base)

if (NOT DISABLE_MEDIA_TEST)
    add_executable(media_player_test
            test/file_data_source_test.cc
            test/demuxer_stream_test.cc
            test/demuxer_test.cc
            )
    target_link_libraries(media_player_test media_player gtest_main gmock_main)
    add_test(NAME media_player_test COMMAND media_player)
    set_tests_properties(media_player_test PROPERTIES
            ENVIRONMENT
            LYCHEE_PROJECT_DIR1="${CMAKE_CURRENT_SOURCE_DIR}/../../")
endif ()

if (APPLE)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/external_media_texture.h
            DESTINATION include)
endif ()