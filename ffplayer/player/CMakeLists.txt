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

install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION lib
        PUBLIC_HEADER DESTINATION include
        )

install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h*")