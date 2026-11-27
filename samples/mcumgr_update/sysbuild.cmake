# SPDX-License-Identifier: Apache-2.0

# Boards without MCUboot partitions in their devicetree: give MCUboot the
# same partitions as the application (boards/<board>.overlay includes the
# same file).
if(BOARD STREQUAL "stm32f4_disco")
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE
       "${CMAKE_CURRENT_LIST_DIR}/boards/stm32f4_disco_partitions.dtsi")
  list(REMOVE_DUPLICATES mcuboot_EXTRA_DTC_OVERLAY_FILE)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE "${mcuboot_EXTRA_DTC_OVERLAY_FILE}"
      CACHE INTERNAL "MCUboot partitions for ${BOARD}")
endif()
