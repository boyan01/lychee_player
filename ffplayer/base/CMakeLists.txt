cmake_minimum_required(VERSION 3.10)

project("media_base")

add_library(media_base
        message_loop.h
        message_loop.cc
        location.h
        location.cc
        message.h
        message.cc
        logging.h
        logging.cc
        basictypes.h
        message_queue.h
        message_queue.cc
        utility.h
        utility.cc
        size.cc size.h rect.cc rect.h timestamps.h)

target_include_directories(media_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})


add_executable(media_base_test
        message_queue_test.cc
        message_loop_test.cc
        )
target_link_libraries(media_base_test media_base gtest_main gmock_main)

add_test(NAME media_base_test COMMAND media_base)