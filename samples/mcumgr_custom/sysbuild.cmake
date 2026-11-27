# SPDX-License-Identifier: Apache-2.0

# The stm32f4_disco devicetree has no MCUboot partitions: give MCUboot the
# same ones as the application (boards/stm32f4_disco.overlay).
if(BOARD STREQUAL "stm32f4_disco")
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE
       "${CMAKE_CURRENT_LIST_DIR}/boards/stm32f4_disco_partitions.dtsi")
  list(REMOVE_DUPLICATES mcuboot_EXTRA_DTC_OVERLAY_FILE)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE "${mcuboot_EXTRA_DTC_OVERLAY_FILE}"
      CACHE INTERNAL "MCUboot partitions for ${BOARD}")
endif()
