/******************************************************************************
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 *         file: theia_motion_.h
 *  description: Motor control header file
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */
#ifndef __THEIA_MOTION_H
#define __THEIA_MOTION_H

#include <linux/cdev.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include "motor_hades_comms.h"

struct theia_motion_pins {
	int reset_gpio;
	int boot_gpio;
	int int_gpio;
	int cs_gpio;
	int power_gpio;
	int power_gpio_flags;
	int irq;
};

struct theia_motion_delays {
	int bootup_delay;
};

struct theia_motion {
	struct spi_device *spi_dev;
	struct device *dev;
	struct class  *drv_class;

	struct theia_motion_pins pins;
	struct theia_motion_delays delays;
	struct timeval tv;
	struct hds_data hds_data;

	dev_t devt;
	struct cdev cdev;
	char *tx_buffer;

	struct delayed_work	wq;
	struct delayed_work	wq_time;
	int seconds_to_sync;

	int polling_per_second;
	char *polling_buffer;
	int polling_index;
	int polling_buffer_size;

	struct mutex polling_lock;

	struct iio_dev *indio_dev;
};

#endif /* __THEIA_MOTION_H */
