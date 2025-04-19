cmake_minimum_required(VERSION 3.13.1)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(websocket_sensor)

FILE(GLOB app_sources src/*.cpp)
target_include_directories(app PRIVATE inc/)
target_sources(app PRIVATE ${app_sources})
