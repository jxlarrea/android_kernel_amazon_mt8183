/******************************************************************************
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 *         file: theia_motion_adc.c
 *  description: Motor adc source file
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

#include <linux/iio/iio.h>
#include "theia_motion.h"
#include "theia_motion_adc.h"

#define MOTOR_THERMISTOR_COMMAND 0x0C12
#define MOTOR_PCB_THERMISTOR_COMMAND 0x0C13

#define THEIA_MOTION_ADC_CHANNEL(idx, command) {		\
		.type = IIO_VOLTAGE,				\
		.indexed = 1,					\
		.channel = (idx),				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
		                      BIT(IIO_CHAN_INFO_SCALE),	\
		.address = command,				\
}

/*
 * This structure maps the motor ADC virtual channels 0 and 1
 * to the Motor_Read commands.
 */

static const struct iio_chan_spec theia_motion_adc_iio_channels[] = {
	THEIA_MOTION_ADC_CHANNEL(0, MOTOR_THERMISTOR_COMMAND),
	THEIA_MOTION_ADC_CHANNEL(1, MOTOR_PCB_THERMISTOR_COMMAND),
};

static int theia_motion_adc_read_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  int *val,
				  int *val2,
				  long info)
{
	struct theia_motion *controller =
					dev_get_drvdata(indio_dev->dev.parent);
	uint16_t length;
	int ret;
	hds_tl_status_t tl_status = HDS_TL_STATUS_OK;
	switch (info) {
	case IIO_CHAN_INFO_RAW:
		ret = Motor_Read(&controller->hds_data, (uint16_t)chan->address,
		                 (uint8_t *)val, &length, sizeof(*val), &tl_status);
		if (ret != HDS_RETCODE_SUCCESS) {
			dev_err(controller->dev, "Motor_Read failed, hds err %d, tl_err %d\n", ret, tl_status);
			return -EIO;
		}
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 1;
		*val2 = 1;
		return IIO_VAL_FRACTIONAL;

	default:
		return -EINVAL;
	}
}

static const struct iio_info theia_motion_adc_info = {
	.read_raw = &theia_motion_adc_read_raw,
};

int theia_motion_adc_init(struct theia_motion *controller) {
	int ret;
	struct iio_dev *indio_dev = iio_device_alloc(0);
	if (!indio_dev)
		return -ENOMEM;

	controller->indio_dev = indio_dev;
	indio_dev->dev.parent = controller->dev;
	indio_dev->name = dev_name(controller->dev);
	indio_dev->info = &theia_motion_adc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = theia_motion_adc_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(theia_motion_adc_iio_channels);

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		dev_err(controller->dev, "iio register failed(%d)\n", ret);

	return ret;
}

void theia_motion_adc_remove(struct theia_motion *controller) {
	if (controller->indio_dev) {
		iio_device_unregister(controller->indio_dev);
		iio_device_free(controller->indio_dev);
	}
}
