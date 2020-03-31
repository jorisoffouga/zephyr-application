cmake_minimum_required(VERSION 3.13.1)

include(../../cmake/boilerplate.cmake)
find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})
project(cdc_uart_led)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
