# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mcumgr_custom)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/led_ctrl.c
  src/pattern.c
)
target_sources_ifdef(CONFIG_APP_STORE app PRIVATE src/app_store.c)

# custom MCUmgr groups
target_sources_ifdef(CONFIG_APP_LED_MGMT      app PRIVATE src/led_mgmt.c)
target_sources_ifdef(CONFIG_APP_BARGRAPH_MGMT app PRIVATE src/bargraph_mgmt.c)
target_sources_ifdef(CONFIG_APP_AIRQ_MGMT     app PRIVATE src/airq_mgmt.c)
target_sources_ifdef(CONFIG_APP_PATTERN_MGMT  app PRIVATE src/pattern_mgmt.c)
target_sources_ifdef(CONFIG_APP_ALCOHOL_MGMT  app PRIVATE src/alcohol_mgmt.c)
target_sources_ifdef(CONFIG_APP_EEPROM_MGMT   app PRIVATE src/eeprom_mgmt.c)
