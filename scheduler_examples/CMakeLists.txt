cmake_minimum_required(VERSION 3.25)
project(scheduling C)

set(CMAKE_C_STANDARD 11)

add_executable(scheduler ossim.c queue.c fifo.c)

add_executable(app app.c)

add_executable(app-io app-io.c burst_queue.c)

# Build the simple Hello World located at the project root
add_executable(hello ../main.c)
