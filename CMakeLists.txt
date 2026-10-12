# Point to ZA_DIR root directory.
set(ZA_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE PATH "ZA_DIR root directory")

zephyr_include_directories(include)

add_subdirectory(drivers)
add_subdirectory(lib)
