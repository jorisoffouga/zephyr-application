cmake_minimum_required(VERSION 3.13.1)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smart_greenhouse)

FILE(GLOB app_sources src/*.c)
include_directories(inc)
target_sources(app PRIVATE ${app_sources})
