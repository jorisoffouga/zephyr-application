cmake_minimum_required(VERSION 3.13.1)

include(../../cmake/boilerplate.cmake)
find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})
project(smart_greenhouse)

FILE(GLOB app_sources src/*.c)
include_directories(inc)
target_sources(app PRIVATE ${app_sources})
