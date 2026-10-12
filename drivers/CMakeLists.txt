# SPDX-License-Identifier: Apache-2.0

if(CONFIG_MFD_PCF8591)
    zephyr_library_named(za_drivers)
    zephyr_library_sources(mfd/mfd_pcf8591.c)
    zephyr_library_sources_ifdef(CONFIG_ADC_PCF8591 adc/adc_pcf8591.c)
    zephyr_library_sources_ifdef(CONFIG_DAC_PCF8591 dac/dac_pcf8591.c)
endif()
