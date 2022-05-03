cmake_minimum_required(VERSION 3.10)

project("lychee_base")

add_library(lychee_base
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
        src/task_runner.cc
        src/time_delta.cc
        src/time_ticks.cc
        )

target_include_directories(lychee_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
