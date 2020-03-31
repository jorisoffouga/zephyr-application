cmake_minimum_required(VERSION 3.13.1)

include(../../cmake/boilerplate.cmake)
find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})
project(NONE)

# Add your source file to the "app" target. This must come after
# the boilerplate code, which defines the target.
target_sources(app PRIVATE src/main.c)
