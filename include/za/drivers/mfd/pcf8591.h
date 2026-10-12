/*
 * Copyright (c) 2026, Joris Offouga
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZA_DRIVERS_MFD_PCF8591_H_
#define ZA_DRIVERS_MFD_PCF8591_H_

#include <stdint.h>
#include <zephyr/device.h>

#define PCF8591_ADC_CHANNELS	4
#define PCF8591_RESOLUTION	8

/**
 * @brief Convert one analog input
 *
 * @param dev PCF8591 MFD device
 * @param channel input, 0 (AIN0) to 3 (AIN3), single-ended
 * @param value conversion result, 0 to 255 (AGND to VREF)
 *
 * @return 0 on success, negative errno otherwise
 */
int mfd_pcf8591_adc_read(const struct device *dev, uint8_t channel,
			 uint8_t *value);

/**
 * @brief Set the analog output, and enable it on the first call
 *
 * @param dev PCF8591 MFD device
 * @param value 0 to 255 (AGND to VREF)
 *
 * @return 0 on success, negative errno otherwise
 */
int mfd_pcf8591_dac_write(const struct device *dev, uint8_t value);

/**
 * @brief VREF in millivolts, the full scale of the inputs and the output
 *
 * @param dev PCF8591 MFD device
 */
uint16_t mfd_pcf8591_vref_mv(const struct device *dev);

#endif /* ZA_DRIVERS_MFD_PCF8591_H_ */
