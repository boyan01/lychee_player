cmake_minimum_required(VERSION 3.10)

project("media_player")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} BASE_SOURCES)

add_library(media_player ${BASE_SOURCES})

target_include_directories(media_player PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ../third_party
        )

target_link_libraries(media_player PUBLIC ${FFP_LIBS})

