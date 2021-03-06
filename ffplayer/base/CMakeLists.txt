cmake_minimum_required(VERSION 3.10)

project("media_base")

set(MEDIA_BASE_LIB_NAME ${PROJECT_NAME})

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} BASE_SOURCES)

add_library(${MEDIA_BASE_LIB_NAME} ${BASE_SOURCES})

target_include_directories(${MEDIA_BASE_LIB_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ../third_party
        )

target_link_libraries(${MEDIA_BASE_LIB_NAME} PUBLIC ${FFP_LIBS})