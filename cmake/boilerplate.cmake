set(ZA_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
# Add ZA_DIR as a BOARD_ROOT in case the board is in ZA_DIR
list(APPEND BOARD_ROOT ${ZA_DIR})

# Add ZA_DIR as a DTS_ROOT to include dts
list(APPEND DTS_ROOT ${ZA_DIR})
