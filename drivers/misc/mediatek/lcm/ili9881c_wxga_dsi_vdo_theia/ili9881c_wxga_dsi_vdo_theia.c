/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#ifdef BUILD_LK
#include <platform/mt_gpio.h>
#include <platform/mt_i2c.h>
#include <platform/mt_pmic.h>
#include <string.h>
#else
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>
#include <asm-generic/gpio.h>

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#endif
#endif

#include <mtk_leds_drv.h>
#include "lcm_drv.h"

#ifndef BUILD_LK
static unsigned int GPIO_LCD_RST;
static struct regulator *lcm_bias_pos;
static struct regulator *lcm_bias_neg;
static int lcm_bias_voltage;
static unsigned char vendor_id;
static unsigned char DDIC_id;
static unsigned char LCM_id;

#define DDIC_ID_JD			0x0
#define DDIC_ID_ILI			0x1
#define DDIC_NULL			0xF
#define LCM_VENDOR_ID_MASK		0xF
#define LCM_VENDOR_ID_SHIFT		0
#define LCM_DDIC_ID_MASK		0xF0
#define LCM_DDIC_ID_SHIFT		4
#define LCM_BIAS_DEFAULT_VOLTAGE_UV	5000000

static void lcm_request_gpio_control(struct device *dev)
{
	int ret = 0;
	dev_dbg(dev, "[Kernel/LCM] %s enter\n", __func__);

	GPIO_LCD_RST = of_get_named_gpio(dev->of_node, "gpio_lcd_rst", 0);
	ret = gpio_request(GPIO_LCD_RST, "GPIO_LCD_RST");
	if (ret)
		dev_warn(dev, "gpio request reset pin = 0x%x fail\n", GPIO_LCD_RST);
}

static int lcm_bias_regulator_probe(struct device *dev)
{
	struct device_node *np = dev_of_node(dev);
	int val;
	lcm_bias_pos = devm_regulator_get(dev, "avdd");

	if (IS_ERR(lcm_bias_pos))
	{
		dev_info(dev, "Get regulator avdd fail, find regulator avdd_alt\n");
		lcm_bias_pos = devm_regulator_get(dev, "avdd_alt");
	}

	if (IS_ERR(lcm_bias_pos))
		dev_warn(dev, "get regulator avdd fail, error: %ld\n", PTR_ERR(lcm_bias_pos));

	lcm_bias_neg = devm_regulator_get(dev, "avee");

	if (IS_ERR(lcm_bias_neg))
	{
		dev_info(dev, "Get regulator avee fail, find regulator avee_alt\n");
		lcm_bias_neg = devm_regulator_get(dev, "avee_alt");
	}

	if (IS_ERR(lcm_bias_neg))
		dev_warn(dev, "get regulator avee fail, error: %ld\n", PTR_ERR(lcm_bias_neg));

	if (!of_property_read_u32(np, "lcm-bias-voltage-uv", &val)) {
		lcm_bias_voltage = val;
	} else {
		lcm_bias_voltage = LCM_BIAS_DEFAULT_VOLTAGE_UV;
	}
	dev_info(dev, "lcm bias voltage %d uV", lcm_bias_voltage);
	return 0;
}

static int lcm_bias_enable(void)
{
	int ret = 0;

	if (!IS_ERR(lcm_bias_pos)) {
		ret = regulator_set_voltage(lcm_bias_pos, lcm_bias_voltage, lcm_bias_voltage);
		if (ret) {
			pr_err("set voltage lcm_bias_pos fail, ret = %d\n", ret);
			return ret;
		}

		ret = regulator_enable(lcm_bias_pos);
		if (ret) {
			pr_err("enable regulator lcm_bias_pos fail, ret = %d\n", ret);
			return ret;
		}
	}

	if (!IS_ERR(lcm_bias_neg)) {
		ret = regulator_set_voltage(lcm_bias_neg, lcm_bias_voltage, lcm_bias_voltage);
		if (ret) {
			pr_err("set voltage lcm_bias_neg fail, ret = %d\n", ret);
			return ret;
		}

		ret = regulator_enable(lcm_bias_neg);
		if (ret) {
			pr_err("enable regulator lcm_bias_neg fail, ret = %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static void lcm_bias_disable(void)
{
	if(!IS_ERR(lcm_bias_neg))
		regulator_disable(lcm_bias_neg);

	if(!IS_ERR(lcm_bias_pos))
		regulator_disable(lcm_bias_pos);
}
static int __init setup_lcm_id(char *str)
{
	int id;

	if (get_option(&str, &id))
	{
		LCM_id = (unsigned char)id;
	}
	return 0;
}
__setup("jd936x_id=", setup_lcm_id);
static void get_lcm_id(void)
{
	vendor_id = (LCM_id & LCM_VENDOR_ID_MASK) >> LCM_VENDOR_ID_SHIFT;
	DDIC_id = (LCM_id & LCM_DDIC_ID_MASK) >> LCM_DDIC_ID_SHIFT;
}
static bool is_ili9881c_lcm_ic(void)
{
	get_lcm_id();
	if(DDIC_id == DDIC_ID_ILI)
		return true;
	else
		return false;
}

static int lcm_driver_probe(struct device *dev, void const *data)
{

	dev_dbg(dev, "[Kernel/LCM] %s enter\n", __func__);
	if(is_ili9881c_lcm_ic() == true)
	{
		lcm_request_gpio_control(dev);
		lcm_bias_regulator_probe(dev);
		lcm_bias_enable();
	}
	return 0;
}

static const struct of_device_id lcm_platform_of_match[] = {
	{
		.compatible = "ili,ili9881c",
		.data = 0,
	}, {
		/* sentinel */
	}
};


MODULE_DEVICE_TABLE(of, platform_of_match);

static int lcm_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	dev_dbg(&pdev->dev, "[Kernel/LCM] %s enter\n", __func__);

	id = of_match_node(lcm_platform_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	return lcm_driver_probe(&pdev->dev, id->data);
}

static struct platform_driver lcm_driver = {
	.probe = lcm_platform_probe,
	.driver = {
		.name = "ili9881c_wxga_dsi_vdo_hawk",
		.owner = THIS_MODULE,
		.of_match_table = lcm_platform_of_match,
	},
};

static int __init lcm_drv_init(void)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);
	if (platform_driver_register(&lcm_driver)) {
		pr_notice("Failed to register this driver!");
		return -ENODEV;
	}

	return 0;
}

static void __exit lcm_drv_exit(void)
{
	platform_driver_unregister(&lcm_driver);
}

late_initcall(lcm_drv_init);
module_exit(lcm_drv_exit);
MODULE_AUTHOR("Mediatek");
MODULE_DESCRIPTION("LCM display subsystem driver");
MODULE_LICENSE("GPL");
#endif

/* ----------------------------------------------------------------- */
/* Local Constants */
/* ----------------------------------------------------------------- */

#define FRAME_WIDTH		(800)
#define FRAME_HEIGHT		(1280)
#define LCM_PHYSICAL_WIDTH_MM	(135)	// Screen width in MM
#define LCM_PHYSICAL_HEIGHT_MM	(217)	// Screen height in MM
#define LCM_PHYSICAL_WIDTH_UM	(LCM_PHYSICAL_WIDTH_MM * 1000)	// Screen width in UM
#define LCM_PHYSICAL_HEIGHT_UM	(LCM_PHYSICAL_HEIGHT_MM * 1000)	// Screen height in UM
#define GPIO_OUT_ONE		1
#define GPIO_OUT_ZERO		0

#define REGFLAG_DELAY		0xFE
#define REGFLAG_END_OF_TABLE		0x00

#define ST_BOE		0x0
#define ST_INX		0x2
#define ST_TG	    0x3

/* ----------------------------------------------------------------- */
/*  Local Variables */
/* ----------------------------------------------------------------- */
static LCM_UTIL_FUNCS lcm_util = { 0 };
#define SET_RESET_PIN(v)		(lcm_util.set_reset_pin((v)))
#define UDELAY(n)				(lcm_util.udelay(n))
#define MDELAY(n)				(lcm_util.mdelay(n))

/* ----------------------------------------------------------------- */
/* Local Functions */
/* ----------------------------------------------------------------- */
#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
		 (lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update))
#define dsi_set_cmdq(pdata, queue_size, force_update) \
		 (lcm_util.dsi_set_cmdq(pdata, queue_size, force_update))
#define wrtie_cmd(cmd) \
		 (lcm_util.dsi_write_cmd(cmd))
#define write_regs(addr, pdata, byte_nums) \
		 (lcm_util.dsi_write_regs(addr, pdata, byte_nums))
#define read_reg \
		 (lcm_util.dsi_read_reg())
#define read_reg_v2(cmd, buffer, buffer_size) \
		 (lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, buffer_size))

static void lcm_set_gpio_output(unsigned int GPIO, unsigned int output)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);
#ifdef BUILD_LK
	mt_set_gpio_mode(GPIO, GPIO_MODE_00);
	mt_set_gpio_dir(GPIO, GPIO_DIR_OUT);
	mt_set_gpio_out(GPIO, output);
#else
	gpio_direction_output(GPIO, output);
	gpio_set_value(GPIO, output);
#endif
}

static void init_ili9881c_inx_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00043902;    
	data_array[1] = 0x038198FF;    
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x53031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x260F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x26101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x401E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xC01F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x8A241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x8A251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x33281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x33291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x082C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x082D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B2E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B2F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x42321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x67531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x89541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xAB551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x67591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x895A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xAB5B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xCD5C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xEF5D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x16691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x166A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D6C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D6D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x067A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x177D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x027E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x167F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x16801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D831500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C841500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C851500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F861500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F871500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E881500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E891500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x028A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x048198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x2B6E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x356F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA43A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1A8D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xBA871500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xD1B21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B881500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07B51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x75311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x983B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x018198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x0A221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x85501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x22601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00A01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x13A11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x21A21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12A31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15A41500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x27A51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1CA61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1EA71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80A81500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1DA91500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2AAA1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x71AB1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1AAC1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x19AD1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4CAE1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24AF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28B01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4AB11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x59B21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23B31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00C01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x13C11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x21C21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12C31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15C41500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x27C51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1CC61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1EC71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80C81500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1DC91500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2ACA1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x71CB1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1ACC1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x19CD1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4CCE1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24CF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28D01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4AD11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x59D21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23D31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x008198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1); 
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(20);

	data_array[0] = 0x00351500;
	dsi_set_cmdq(data_array, 1, 1);

}

static void init_ili9881c_tg_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00043902;
	data_array[1] = 0x038198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x72031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000d1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001d1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x401e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x801f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x33281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x22291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002d1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003d1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00441500;
	dsi_set_cmdq(data_array, 1, 1);

	//GIP_2
	data_array[0] = 0x01501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x67531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x89541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xab551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x67591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x895a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xab5b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xcd5c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xef5d1500;
	dsi_set_cmdq(data_array, 1, 1);

	//GIP_3
	data_array[0] = 0x115e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x14611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026d1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x076e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x14771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F7a1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C7b1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D7c1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x067d1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x027e1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x027f1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02831500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07841500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02851500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02861500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02871500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02881500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02891500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x028A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x048198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x156C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3B6E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x756F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x243A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x148D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xBA871500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x76261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xD1B21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07B51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x983B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x25311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x018198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x0A221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x36531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x36551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xC8501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xC4511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x25601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00A01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x16A11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24A21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15A31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1AA41500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2AA51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1EA61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1DA71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x92A81500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1EA91500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x29AA1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x82AB1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20AC1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1FAD1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x55AE1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28AF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2DB01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52B11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5CB21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x29B31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00C01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x14C11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24C21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12C31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x13C41500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28C51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1DC61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x22C71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x8DC81500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1CC91500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x29CA1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x85CB1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1CCC1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20CD1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x53CE1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x29CF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2BD01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x51D11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5FD21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2CD31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x008198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1); 
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(20);

	data_array[0] = 0x00351500;
	dsi_set_cmdq(data_array, 1, 1);

}

static void init_ili9881c_boe_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00043902;    
	data_array[1] = 0x038198FF;    
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x53031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x000E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x260F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x26101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x401E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xC01F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x8A241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x8A251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x33281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x33291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x002B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x082C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x082D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B2E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B2F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x42321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x003F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x67531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x89541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xAB551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x67591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x895A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xAB5B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xCD5C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xEF5D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x16691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x166A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x026B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D6C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D6D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x067A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x177D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x027E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x167F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x16801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D831500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C841500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C851500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F861500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F871500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E881500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E891500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x028A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x048198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x2B6E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x356F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA43A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1A8D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xBA871500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xD1B21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B881500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07B51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x75311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x983B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x018198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x0A221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x85501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x22601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00A01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x13A11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x21A21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12A31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15A41500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x27A51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1CA61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1EA71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80A81500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1DA91500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2AAA1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x71AB1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1AAC1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x19AD1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4CAE1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24AF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28B01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4AB11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x59B21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23B31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00C01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x13C11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x21C21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12C31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x15C41500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x27C51500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1CC61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1EC71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80C81500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1DC91500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2ACA1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x71CB1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1ACC1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x19CD1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4CCE1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24CF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28D01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4AD11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x59D21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23D31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00043902;
	data_array[1] = 0x008198FF;
	dsi_set_cmdq(data_array, 2, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1); 
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(20);

	data_array[0] = 0x00351500;
	dsi_set_cmdq(data_array, 1, 1);

}

static void init_ili9881c_lcm(void)
{
	get_lcm_id();
	if (vendor_id == ST_INX)
		init_ili9881c_inx_lcm();		/* ili9881c INX panel */
	else if (vendor_id == ST_TG)
		init_ili9881c_tg_lcm();		/* ili9881c KD panel, INX glass */
	else
		init_ili9881c_boe_lcm();			/* Default ili9881c BOE panel */

	pr_debug("[Kernel/LCM] ili9881c vendor_id %d\n", vendor_id);
}

/* ----------------------------------------------------------------- */
/* LCM Driver Implementations */
/* ----------------------------------------------------------------- */
static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));
	get_lcm_id();
	params->type   = LCM_TYPE_DSI;

	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_MM;
	params->physical_height = LCM_PHYSICAL_HEIGHT_MM;
	params->physical_width_um = LCM_PHYSICAL_WIDTH_UM;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT_UM;

	params->dsi.mode   = SYNC_EVENT_VDO_MODE;

	params->dsi.LANE_NUM				= LCM_FOUR_LANE;
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq	= LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding	= LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format	= LCM_DSI_FORMAT_RGB888;

	params->dsi.packet_size = 256;

	params->dsi.intermediat_buffer_num = 0;

	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;

	if (vendor_id == ST_TG) {
		pr_notice("[Kernel/LCM] %s panel\n", "TG");
		params->dsi.vertical_sync_active		= 4;
		params->dsi.vertical_backporch			= 14;
		params->dsi.vertical_frontporch 		= 30;
		params->dsi.vertical_active_line		= FRAME_HEIGHT;

		params->dsi.horizontal_sync_active		= 40;
		params->dsi.horizontal_backporch		= 59;
		params->dsi.horizontal_frontporch		= 30;
		params->dsi.horizontal_active_pixel	= FRAME_WIDTH;

		params->dsi.PLL_CLOCK = 247;

	} else if (vendor_id == ST_INX) {
		pr_notice("[Kernel/LCM] %s panel\n", "INX");
		params->dsi.vertical_sync_active		= 4;
		params->dsi.vertical_backporch			= 14;
		params->dsi.vertical_frontporch 		= 30;
		params->dsi.vertical_active_line		= FRAME_HEIGHT;

		params->dsi.horizontal_sync_active		= 40;
		params->dsi.horizontal_backporch		= 59;
		params->dsi.horizontal_frontporch		= 30;
		params->dsi.horizontal_active_pixel	= FRAME_WIDTH;

		params->dsi.PLL_CLOCK = 247;
	} else {
		pr_notice("[Kernel/LCM] %s panel\n", "BOE");
		params->dsi.vertical_sync_active		= 4;
		params->dsi.vertical_backporch			= 14;
		params->dsi.vertical_frontporch			= 30;
		params->dsi.vertical_active_line                = FRAME_HEIGHT;

		params->dsi.horizontal_sync_active              = 40;
		params->dsi.horizontal_backporch                = 59;
		params->dsi.horizontal_frontporch               = 30;
		params->dsi.horizontal_active_pixel     = FRAME_WIDTH;
		params->dsi.PLL_CLOCK = 247;
	}
	params->dsi.vertical_frontporch_for_low_power = 295;
	params->dsi.clk_lp_per_line_enable = 1;
	params->dsi.cont_clock = 0;
	params->dsi.DA_HS_EXIT = 10;
	params->dsi.CLK_ZERO = 16;
	params->dsi.HS_ZERO = 10;
	params->dsi.HS_TRAIL = 5;
	params->dsi.HS_PRPR = 6;
	params->dsi.CLK_TRAIL = 5;
	params->dsi.CLK_HS_POST = 10;
	params->dsi.CLK_HS_EXIT = 6;
	params->dsi.CLK_HS_PRPR = 4;

	params->dsi.TA_GO = 8;
	params->dsi.TA_GET = 10;

	params->physical_width = 108;
	params->physical_height = 172;
}

static void lcm_init(void)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);
	init_ili9881c_lcm();
}

static void lcm_resume(void)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);
	if (vendor_id == ST_TG) {
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
		MDELAY(5);
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ZERO);
		MDELAY(10);
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
		MDELAY(200);
	} else if(vendor_id == ST_BOE) {
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
		MDELAY(10);
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ZERO);
		MDELAY(2);
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
		MDELAY(200);
	} else {
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
		MDELAY(10);
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ZERO);
		MDELAY(2);
		lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
		MDELAY(6);
	}
	init_ili9881c_lcm(); /* TPV panel */
}

static void lcm_init_power(void)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);

	lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
	MDELAY(10);
	lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ZERO);
	MDELAY(2);
	lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
	MDELAY(5);
}

static void lcm_resume_power(void)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);

	lcm_bias_enable();
	/* tRPWIRES + tRESETL = 5ms + 1ms = 6ms */
	MDELAY(6);
	lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ONE);
}

static void lcm_suspend(void)
{
	unsigned int data_array[16];


	pr_notice("[Kernel/LCM] %s enter\n", __func__);

	data_array[0] = 0x00280500; /* Display Off */
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(50);

	data_array[0] = 0x00100500; /* Sleep In */
	dsi_set_cmdq(data_array, 1, 1);

	MDELAY(120);
}

static void lcm_suspend_power(void)
{
	pr_notice("[Kernel/LCM] %s enter\n", __func__);

	lcm_set_gpio_output(GPIO_LCD_RST, GPIO_OUT_ZERO);
	MDELAY(120);

	lcm_bias_disable();
	MDELAY(20);
}

static void lcm_set_backlight_cmdq(void *handle, unsigned int level)
{
	backlight_brightness_set(level);
}


LCM_DRIVER ili9881c_wxga_dsi_vdo_theia_lcm_drv = {
	.name		= "ili9881c_wxga_dsi_vdo_theia",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params     = lcm_get_params,
	.init           = lcm_init,
	.init_power	= lcm_init_power,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight_cmdq	= lcm_set_backlight_cmdq,
};


