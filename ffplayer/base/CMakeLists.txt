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
        src/channel_layout.cc
        src/circular_deque.cc
        src/task_runner.cc)

target_include_directories(media_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)


if (NOT DISABLE_MEDIA_TEST)
    add_executable(media_base_test
            test/message_queue_test.cc
            test/message_loop_test.cc
            test/circular_deque_test.cc
            test/task_runner_test.cc
            )
    target_link_libraries(media_base_test media_base gtest_main gmock_main)

    add_test(NAME media_base_test COMMAND media_base)
endif ()
