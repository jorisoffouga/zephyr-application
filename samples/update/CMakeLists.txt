cmake_minimum_required(VERSION 3.13.1)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Update)

target_include_directories(app PRIVATE $ENV{ZEPHYR_BASE}/lib/updatehub/include)
target_sources(app PRIVATE src/main.c)

if(CONFIG_UPDATEHUB_DTLS)
    foreach(inc_file
            servercert.der
            privkey.der
            )
        generate_inc_file_for_target(
            ${ZEPHYR_CURRENT_LIBRARY}
            src/certificates/${inc_file}
            ${gen_dir}/${inc_file}.inc
            )
    endforeach()
endif()
