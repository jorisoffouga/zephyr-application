cmake_minimum_required(VERSION 3.13.1)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

include(../../cmake/boilerplate.cmake)

project(blinky)

target_sources(app PRIVATE src/main.c)
