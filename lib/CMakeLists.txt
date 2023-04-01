if(CONFIG_CJSON)
    zephyr_include_directories(cJSON)
    zephyr_library()
    zephyr_library_sources(
        cJSON/cJSON.c
        cJSON/cJSON_Utils.c
    )
endif()
