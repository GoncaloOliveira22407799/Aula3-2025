cmake_minimum_required(VERSION 3.25)
project(scheduler_examples C)

set(CMAKE_C_STANDARD 11)

add_executable(ossim
        ossim.c
        fifo.c
        rr.c
        sjf.c
        mlfq.c
        queue.c
        burst_queue.c
)

# Expor funções POSIX como sleep/usleep/strdup
target_compile_definitions(ossim PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)

# Aplicações simples (usadas pelos scripts)
add_executable(app
        app.c
)

add_executable(app-io
        app-io.c
        burst_queue.c
)

target_compile_definitions(app-io PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)