cmake_minimum_required(VERSION 3.13)

# Copy pico_sdk_import.cmake from $PICO_SDK_PATH/external/ into this folder first.
include(pico_sdk_import.cmake)

project(mini_smu C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(mini_smu main.c)
target_link_libraries(mini_smu pico_stdlib hardware_i2c)

pico_enable_stdio_usb(mini_smu 1)
pico_enable_stdio_uart(mini_smu 0)
pico_add_extra_outputs(mini_smu)
