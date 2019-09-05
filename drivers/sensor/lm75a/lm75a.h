/*
 * Copyright (c) 2019 Joris Offouga
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_LM75A_H_
#define ZEPHYR_DRIVERS_SENSOR_LM75A_H_

#include <zephyr/types.h>
#include <gpio.h>

struct lm75a_config {
	char *i2c_name;
#ifdef CONFIG_LM75A_TRIGGER
	char *gpio_name;
	u8_t gpio_pin;
#endif
	u8_t i2c_address;
};


struct lm75a_data {
	struct device *i2c;
	s16_t temp;

#ifdef CONFIG_LM75A_TRIGGER
	struct device *gpio;
	struct gpio_callback gpio_cb;

	sensor_trigger_handler_t drdy_handler;
	struct sensor_trigger drdy_trigger;

	sensor_trigger_handler_t th_handler;
	struct sensor_trigger th_trigger;

#if defined(CONFIG_LM75A_TRIGGER_OWN_THREAD)
	K_THREAD_STACK_MEMBER(thread_stack, CONFIG_LM75A_THREAD_STACK_SIZE);
	struct k_sem gpio_sem;
	struct k_thread thread;
#elif defined(CONFIG_LM75A_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
	struct device *dev;
#endif

#endif /* CONFIG_LM75A_TRIGGER */
};


int lm75a_reg_read(struct device *dev, u8_t reg);

int lm75a_reg_write(struct device *dev, u8_t reg, u16_t val);

int lm75a_attr_set(struct device *dev,
		    enum sensor_channel chan,
		    enum sensor_attribute attr,
		    const struct sensor_value *val);

int lm75a_trigger_set(struct device *dev,
		       const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler);

int lm75a_init_interrupt(struct device *dev);

#endif
