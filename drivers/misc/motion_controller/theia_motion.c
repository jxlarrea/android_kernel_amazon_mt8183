/******************************************************************************
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 *         file: theia_motion_.c
 *  description: Motor control source file
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
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include "theia_motion.h"
#include "theia_motion_adc.h"
#include <linux/motion_controller/ioctl.h>

#define THEIA_MOTION_DEV_MAJOR 101
#define THEIA_MOTION_DEV_MINOR 1

#define THEIA_MOTION_DEV_NODE_NAME "theia-motion"
#define THEIA_MOTION_DEV_CLASS_NAME "theia-motion"

#define MIN_BYTE_NUM  2

#define HDS_RET_ERROR_CODE_BASE 3000
#define HDS_TL_ERROR_CODE_BASE  4000

static int get_error_code(int hds_retcode, int tl_status)
{
	int ret;
	if (hds_retcode == HDS_RETCODE_TL_FAILURE )
		ret = HDS_TL_ERROR_CODE_BASE + tl_status;
	else if (hds_retcode == HDS_RETCODE_SUCCESS)
		ret = HDS_RETCODE_SUCCESS;
	else
		ret = HDS_RET_ERROR_CODE_BASE + hds_retcode;
	return -ret;
}


static ssize_t mcu_rst_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	int enable;
	struct theia_motion *controller = dev_get_drvdata(dev);

	sscanf(buf, "%d", &enable);
	gpio_set_value(controller->pins.reset_gpio, enable);

	return count;
}

static ssize_t mcu_rst_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	ssize_t count;
	struct theia_motion *controller = dev_get_drvdata(dev);

	count = sprintf(buf, "%i\n", gpio_get_value(controller->pins.reset_gpio));

	return count;
}

static DEVICE_ATTR_RW(mcu_rst);

static ssize_t mcu_boot_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	int enable;
	struct theia_motion *controller = dev_get_drvdata(dev);

	sscanf(buf, "%d", &enable);
	gpio_set_value(controller->pins.boot_gpio, enable);

	return count;
}

static ssize_t mcu_boot_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	ssize_t count;
	struct theia_motion *controller = dev_get_drvdata(dev);

	count = sprintf(buf, "%i\n", gpio_get_value(controller->pins.boot_gpio));

	return count;
}

static DEVICE_ATTR_RW(mcu_boot);

static ssize_t mcu_power_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	int enable;
	struct theia_motion *controller = dev_get_drvdata(dev);

	sscanf(buf, "%d", &enable);
	gpio_set_value(controller->pins.power_gpio, enable);

	return count;
}

static ssize_t mcu_power_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	ssize_t count;
	struct theia_motion *controller = dev_get_drvdata(dev);

	count = sprintf(buf, "%i\n", gpio_get_value(controller->pins.power_gpio));

	return count;
}

static DEVICE_ATTR_RW(mcu_power);

static irqreturn_t irq_handler_fn(int irq, void *data)
{
	struct theia_motion *controller = (struct theia_motion *)data;

	SignalCompletion(&controller->hds_data.cts);

	wake_up(&controller->hds_data.cts.wait_queue);

	return IRQ_HANDLED;
}

static int theia_motion_get_gpio(struct theia_motion *controller,
	const char *name, const char *request, int* flags)
{
	int ret;
	int gpio;

	gpio = of_get_named_gpio_flags(controller->dev->of_node,
			name,
			0,
			(enum of_gpio_flags *)flags);
	if (gpio >= 0) {
		ret = gpio_request_one(gpio, GPIOF_OUT_INIT_LOW, request);
		if (ret < 0) {
			dev_err(controller->dev, "%s: failed to request %s: %d\n",
				__func__, request, ret);
			return ret;
		}
	} else {
		dev_err(controller->dev, "%s: No %s for this platform: %d\n",
			__func__, request, gpio);
	}
	return gpio;
}

static int theia_motion_get_motion_pins(struct theia_motion *controller)
{
	int ret;

	controller->pins.boot_gpio = theia_motion_get_gpio(controller,
		"amzn,motion-boot-gpio", "motion-boot-gpio", NULL);
	if (controller->pins.boot_gpio < 0)
		dev_err(controller->dev, "failed set motion-boot-gpio\n");
	else
		gpio_set_value(controller->pins.boot_gpio, 0);

	controller->pins.cs_gpio = theia_motion_get_gpio(controller,
		"amzn,motion-cs-gpio", "motion-cs-gpio", NULL);
	if (controller->pins.cs_gpio < 0)
		return controller->pins.cs_gpio;

	gpio_set_value(controller->pins.cs_gpio, 1);

	controller->pins.power_gpio = theia_motion_get_gpio(controller,
		"amzn,motion-power-gpio", "motion-power-gpio",
		&controller->pins.power_gpio_flags);
	if (controller->pins.power_gpio < 0)
		return controller->pins.power_gpio;

	controller->pins.reset_gpio = theia_motion_get_gpio(controller,
		"amzn,motion-reset-gpio", "motion-reset-gpio", NULL);
	if (controller->pins.reset_gpio < 0)
		return controller->pins.reset_gpio;

	controller->pins.int_gpio = theia_motion_get_gpio(controller,
		"amzn,motion-int-gpio", "motion-int-gpio", NULL);
	if (controller->pins.int_gpio < 0)
		return controller->pins.int_gpio;
	gpio_direction_input(controller->pins.int_gpio);

	controller->pins.irq = gpio_to_irq(controller->pins.int_gpio);
	if (controller->pins.irq < 0) {
		dev_err(controller->dev, "%s: gpio_to_irq failed\n", __func__);
		return controller->pins.irq;
	}

	ret = devm_request_irq(controller->dev, controller->pins.irq, irq_handler_fn,
				IRQF_TRIGGER_RISING, "motion_irq", (void*)controller);
	if (ret) {
		dev_err(controller->dev, "%s: failed to request irq for %d\n",
			__func__, controller->pins.irq);
		return ret;
	}

	return ret;
}

static int theia_motion_init(struct theia_motion *controller)
{
	int ret = 0;

	ret = theia_motion_get_motion_pins(controller);
	if (ret < 0)
		goto error;

	ret = of_property_read_u32(controller->dev->of_node, "bootup-delay-ms",
		&controller->delays.bootup_delay);
	if (ret < 0) {
		controller->delays.bootup_delay = 0;
		dev_err(controller->dev, "No bootup-delay-ms\n");
		goto error;
	}

	ret = of_property_read_u32(controller->dev->of_node, "response_timeout_us",
		&controller->hds_data.response_timeout_us);
	if (ret < 0) {
		controller->hds_data.response_timeout_us = 0;
		dev_err(controller->dev, "No response_timeout_us\n");
		goto error;
	}

	ret = of_property_read_u32(controller->dev->of_node, "seconds_to_sync",
		&controller->seconds_to_sync);
	if (ret < 0) {
		controller->seconds_to_sync = 0;
		dev_err(controller->dev, "No seconds_to_sync\n");
		goto error;
	}

	ret = of_property_read_u32(controller->dev->of_node, "polling_per_second",
		&controller->polling_per_second);
	if (ret < 0) {
		controller->polling_per_second = 0;
		dev_err(controller->dev, "No polling_per_second\n");
		goto error;
	}

	ret = of_property_read_u32(controller->dev->of_node, "polling_buffer_size",
		&controller->polling_buffer_size);
	if (ret < 0) {
		controller->polling_buffer_size = HDS_MAX_TL_PAYLOAD_B;
		dev_err(controller->dev, "No polling_buffer_size in dts, set to default\n");
	}

	controller->polling_buffer = kmalloc(controller->polling_buffer_size, GFP_KERNEL);
	if (!controller->polling_buffer) {
		dev_err(controller->dev, "cannot alloc controller->polling_buffer\n");
		ret = -ENOMEM;
		goto error;
	}

	/* normal boot */
	if (controller->pins.power_gpio_flags & OF_GPIO_ACTIVE_LOW)
		gpio_set_value(controller->pins.power_gpio, 0);
	else
		gpio_set_value(controller->pins.power_gpio, 1);

	/* reset the MCU */
	gpio_set_value(controller->pins.reset_gpio, 0);
	mdelay(controller->delays.bootup_delay);
	gpio_set_value(controller->pins.reset_gpio, 1);

error:
	return ret;
}

/*-------------------------------------------------------------------------*/
static ssize_t
theia_dev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct theia_motion *controller;
	ssize_t	status = count;
	unsigned long missing;
	controller = filp->private_data;
	dev_info(controller->dev, "theia_dev_read available (%d)\n",
		controller->polling_index);

	if (count > HDS_MAX_TL_PAYLOAD_B)
		return -EMSGSIZE;

	mutex_lock(&controller->polling_lock);

    status = (count > controller->polling_index) ? controller->polling_index : count;
	if (status > 0) {
		missing = copy_to_user(buf, controller->polling_buffer, status);
		if (missing == status)
			status = -EFAULT;
		else
			status = status - missing;
	}

	controller->polling_index = 0;
	mutex_unlock(&controller->polling_lock);
	return status;
}

static ssize_t
theia_dev_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct theia_motion *controller;
	ssize_t	status = 0;
	unsigned long missing;
	hds_retcode_t rc;
	uint16_t length = (uint16_t) count;
	uint16_t *channel;
	hds_tl_status_t tl_status = HDS_TL_STATUS_OK;

	if ((count > HDS_MAX_TL_PAYLOAD_B) || (count < MIN_BYTE_NUM)) {
		return -EMSGSIZE;
    }

	controller = filp->private_data;

	missing = copy_from_user(controller->tx_buffer, buf, count);
	if (missing == 0) {
		channel = (uint16_t *)controller->tx_buffer;
	    rc = Motor_Write(&controller->hds_data,
			     *channel,
			     controller->tx_buffer + 2,
			     length - 2,
			     &tl_status);
	    if (rc != HDS_RETCODE_SUCCESS)
	    {
		dev_err(controller->dev, "theia_dev_write hds err %d, tl error %d", rc, tl_status);
		return get_error_code(rc, tl_status);
	    }
	    status = count;
	} else
		status = -EFAULT;

	return status;
}

static int ioctl_readwrite_device(struct theia_motion *controller, void __user *uptr, motion_protocol_cmd_type cmd)
{
	int ret = 0;
	struct sensor_readwrite_data data;
	struct motor_passthrough_packet *motor;
	uint16_t length;
	char *buffer;
	hds_tl_status_t tl_status = HDS_TL_STATUS_OK;

	ret = copy_from_user((void *)&data, (void *)uptr, sizeof(data));
	if (ret) {
		dev_err(controller->dev, "%s: failed to copy %d bytes from user\n", __func__, ret);
		return -EIO;
	}

	if (!data.buff_size) {
		dev_err(controller->dev, "%s: buffer cannot be size 0", __func__);
		return -EINVAL;
	}

	buffer = kmalloc(data.buff_size, GFP_KERNEL);
	if (!buffer) {
		dev_err(controller->dev, "%s: failed to malloc %d bytes\n",
			__func__, data.buff_size);
		return -ENOMEM;
	}

	ret = copy_from_user((void *)buffer, (void *)data.buff, data.buff_size);
	if (ret) {
		dev_err(controller->dev, "%s: failed to copy %d bytes from user\n", __func__, ret);
		ret = -EIO;
		goto error;
	}

	motor = (struct motor_passthrough_packet *) buffer;


    if (motor->length > MOTOR_PASSTHROUGH_MAX_LENGTH) {
		ret = -EINVAL;
		goto error;
	}

	if (cmd == CMD_WRITE) {
		ret = Motor_Write(&controller->hds_data, motor->command, motor->data,
				motor->length, &tl_status);
		if (ret != HDS_RETCODE_SUCCESS) {
			dev_err(controller->dev, "%s: Motor_Write failed, hds err %d, tl err %d, %d\n",__func__, ret, tl_status, get_error_code(ret, tl_status));
			ret = get_error_code(ret, tl_status);
			goto error;
		}
	}
	else if (cmd == CMD_READ) {
		ret = Motor_Read(&controller->hds_data, motor->command, motor->data, &length,
				motor->length, &tl_status);
		if (ret != HDS_RETCODE_SUCCESS) {
			dev_err(controller->dev, "%s: Motor_Read failed, hds err %d, tl err %d, %d\n",__func__, ret, tl_status, get_error_code(ret, tl_status));
			ret = get_error_code(ret, tl_status);
			goto error;
		}

		motor->length = length;
		ret = copy_to_user((void *) data.buff, (void *) buffer, data.buff_size);
		if (ret) {
			dev_err(controller->dev, "%s: failed to copy %d bytes to user, total %d\n",
				__func__, ret, length);
			ret = -EIO;
			goto error;
		}
	}

error:
	kfree(buffer);
	return ret;
}

static long
theia_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *uptr = (void __user *) arg;
	struct theia_motion *controller = filp->private_data;

	switch (cmd) {
		case MOTION_IOCTL_READ_DEVICE:
			return ioctl_readwrite_device(controller, uptr, CMD_READ);
		case MOTION_IOCTL_WRITE_DEVICE:
			return ioctl_readwrite_device(controller, uptr, CMD_WRITE);
		default:
			dev_err(controller->dev, "%s: ioctl not implemented\n", __func__);
			break;
	}

	return -EINVAL;
}

#ifdef CONFIG_COMPAT
static long
theia_dev_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return theia_dev_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#else
#define theia_dev_compat_ioctl NULL
#endif /* CONFIG_COMPAT */

static int theia_dev_open(struct inode *inode, struct file *filp)
{
	struct theia_motion *controller = container_of(inode->i_cdev,
		struct theia_motion, cdev);

	filp->private_data = controller;

	nonseekable_open(inode, filp);

	return 0;
}

static int theia_dev_release(struct inode *inode, struct file *filp)
{
	struct theia_motion *controller;

	controller = filp->private_data;
	filp->private_data = NULL;

	return 0;
}

static const struct file_operations theia_dev_fops = {
	.owner =	THIS_MODULE,
	.write =	theia_dev_write,
	.read =		theia_dev_read,
	.unlocked_ioctl = theia_dev_ioctl,
	.compat_ioctl = theia_dev_compat_ioctl,
	.open =		theia_dev_open,
	.release =	theia_dev_release,
	.llseek =	no_llseek,
};

/* Handlers used for hades host comms protocol */
static hds_host_handlers_t handlers = {

    .is_phy_blocking    = true,
    .phy_write          = Motor_Hades_PHY_Write,
    .phy_read           = Motor_Hades_PHY_Read,
    .phy_cts_read       = Motor_Hades_CTS_Read,
    .phy_sof            = Motor_Hades_SOF,
    .phy_eof            = Motor_Hades_EOF,
    .ll_chksum          = motion_motor_comms_checksum,
};

#if 0
static void theia_poll_work_handler(struct work_struct *w)
{
	uint16_t length;
	size_t count;
	struct delayed_work *work = container_of(w, struct delayed_work, work);
	struct theia_motion *controller = container_of(work, struct theia_motion, wq);

	/* dev_info(controller->dev, "%s:\n", __func__); */
	mutex_lock(&controller->polling_lock);
	/* count to be fixed later */
	count = 32;
	if (controller->polling_index + count > controller->polling_buffer_size) {
		dev_err(controller->dev, "%s: buffer overflow %d\n", __func__,
			controller->polling_index);
		goto exit;
	}

	Motor_Read(&controller->hds_data,
	       GET_STATUS,
	       controller->polling_buffer + controller->polling_index,
	       &length,
	       count);

	controller->polling_index += length;

exit:
	mutex_unlock(&controller->polling_lock);
	schedule_delayed_work(&controller->wq, HZ / controller->polling_per_second);
}

static void theia_time_sync_handler(struct work_struct *w)
{
	struct delayed_work *work = container_of(w, struct delayed_work, work);
	struct theia_motion *controller = container_of(work, struct theia_motion, wq_time);
	hds_retcode_t ret;

	dev_info(controller->dev, "%s: starting time sync\n", __func__);
	do_gettimeofday(&controller->tv);

	ret = Motor_Write(&controller->hds_data,
					TIME_SYNC,
					(const uint8_t *) &controller->tv,
					sizeof(controller->tv));
	if (ret != HDS_RETCODE_SUCCESS) {
		dev_err(controller->dev, "%s: time sync failed err=%d\n", __func__, ret);
	}

	schedule_delayed_work(&controller->wq_time, controller->seconds_to_sync * HZ);
}
#endif

static int theia_motion_probe(struct spi_device *spi)
{
	struct theia_motion *controller;
	int ret;
	hds_retcode_t err;
	struct device *device;

	ret = -ENOMEM;
	controller = devm_kzalloc(&spi->dev, sizeof(struct theia_motion),
					GFP_KERNEL);
	if (!controller)
		return ret;

	/*
	 * All use the same physical device.
	 */
	controller->dev = &spi->dev;
	controller->spi_dev = spi;

	/* spi_set_drvdata(spi, controller); */
	dev_set_drvdata(controller->dev, controller);
	ret = device_create_file(controller->dev, &dev_attr_mcu_rst);
	if (ret) {
		dev_err(controller->dev, "Cannot create mcu_rst sysfs file.\n");
		goto error;
	}

	ret = device_create_file(controller->dev, &dev_attr_mcu_boot);
	if (ret) {
		dev_err(controller->dev, "Cannot create mcu_boot sysfs file.\n");
		goto error;
	}

	ret = device_create_file(controller->dev, &dev_attr_mcu_power);
	if (ret) {
		dev_err(controller->dev, "Cannot create mcu_power sysfs file.\n");
		goto error;
	}

	InitCompletion(&controller->hds_data.cts);
	init_waitqueue_head(&controller->hds_data.cts.wait_queue);
	mutex_init(&controller->hds_data.transfer_lock);
	mutex_init(&controller->polling_lock);

	ret = theia_motion_init(controller);
	if (ret) {
		dev_err(controller->dev, "%s: failed to initialize motion mcu %d\n",
					 __func__, ret);
		goto error;
	}

    /* Hades comms init */
    err = hds_host_init(&controller->hds_data.hds, controller->dev, &handlers,
		       HDS_MAX_TL_PAYLOAD_B);
	if (err) {
		dev_err(controller->dev, "%s: failed to initialize hds_host_init %d\n",
					 __func__, err);
		goto error;
	}

	controller->devt = MKDEV(THEIA_MOTION_DEV_MAJOR, THEIA_MOTION_DEV_MINOR);
	ret = cdev_add(&controller->cdev, controller->devt, 1);
	cdev_init(&controller->cdev, &theia_dev_fops);

	controller->drv_class = class_create(THIS_MODULE, THEIA_MOTION_DEV_CLASS_NAME);
	if (IS_ERR(controller->drv_class)) {
		dev_err(controller->dev,"Unable to create class.\n");
		goto error;
	} else {
		device = device_create(controller->drv_class, NULL, controller->devt, NULL,
					THEIA_MOTION_DEV_NODE_NAME);
		if (IS_ERR(device)) {
			dev_err(controller->dev,"Unable to create device node.\n");
			goto class_create_error;
		}
	}

#if 0
	/* For reading the data from motion mcu */
	INIT_DELAYED_WORK(&controller->wq, theia_poll_work_handler);
	schedule_delayed_work(&controller->wq, HZ / controller->polling_per_second);

	/* For synchronizing time to motion mcu */
	INIT_DELAYED_WORK(&controller->wq_time, theia_time_sync_handler);
	schedule_delayed_work(&controller->wq_time, controller->seconds_to_sync * HZ);
#endif

	/* Move the tx buffer allocation to probe from open */
	controller->tx_buffer = kmalloc(HDS_MAX_TL_PAYLOAD_B, GFP_KERNEL);
	if (!controller->tx_buffer) {
		dev_err(controller->dev, "open/ENOMEM\n");
		goto class_create_error;
	}

	ret = theia_motion_adc_init(controller);
	if (ret < 0)
		dev_err(&spi->dev, "motion adc device init failed(%d)\n", ret);

	dev_info(controller->dev, "theia_motion_probe done\n");
	return 0;

class_create_error:
	class_destroy(controller->drv_class);
error:
	device_remove_file(controller->dev, &dev_attr_mcu_rst);
	device_remove_file(controller->dev, &dev_attr_mcu_boot);
	device_remove_file(controller->dev, &dev_attr_mcu_power);
	return -ENOMEM;
}

static int theia_motion_remove(struct spi_device *spi)
{
	struct theia_motion *controller = dev_get_drvdata(&spi->dev);

	dev_info(controller->dev, "shutting down motion controller\n");

	cancel_delayed_work_sync(&controller->wq);
	cancel_delayed_work_sync(&controller->wq_time);
	flush_scheduled_work();
	mutex_destroy(&controller->hds_data.transfer_lock);
	mutex_destroy(&controller->polling_lock);

	kfree(controller->polling_buffer);
	controller->polling_buffer = NULL;

	kfree(controller->tx_buffer);
	controller->tx_buffer = NULL;

	cdev_del(&controller->cdev);
	device_remove_file(controller->dev, &dev_attr_mcu_rst);
	device_remove_file(controller->dev, &dev_attr_mcu_boot);
	device_remove_file(controller->dev, &dev_attr_mcu_power);


	theia_motion_adc_remove(controller);
	device_destroy(controller->drv_class, controller->devt);
	class_destroy(controller->drv_class);
	return 0;
}

static struct of_device_id theia_motion_match_table[] = {
	{.compatible = "amzn,theia-motion",},
	{ },
};


static struct spi_driver theia_motion_driver = {
	.driver = {
		.name    = "theia_motion",
		.owner   = THIS_MODULE,
		.of_match_table = of_match_ptr(theia_motion_match_table),
	},
	.probe      = theia_motion_probe,
	.remove     = theia_motion_remove,
};

module_spi_driver(theia_motion_driver);
MODULE_DESCRIPTION("Theia Motion System Driver");
MODULE_AUTHOR("Yi Li");
MODULE_LICENSE("GPL");
