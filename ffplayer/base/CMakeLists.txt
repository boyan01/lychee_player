cmake_minimum_required(VERSION 3.10)

project("media_base")

add_library(media_base
        src/message_loop.cc
        src/location.cc
        src/message.cc
        src/logging.cc
        src/message_queue.cc
        src/utility.cc
        src/size.cc
        src/rect.cc
        )

target_include_directories(media_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)


add_executable(media_base_test
        test/message_queue_test.cc
        test/message_loop_test.cc
        )
target_link_libraries(media_base_test media_base gtest_main gmock_main)

add_test(NAME media_base_test COMMAND media_base)