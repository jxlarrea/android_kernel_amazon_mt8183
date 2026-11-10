/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
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
#include <linux/leds.h>

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#endif

#include <upmu_common.h>

/* mt8183 set backlight */
#include "mtk_leds_sw.h"

#include "lcm_drv.h"
#define FRAME_WIDTH  (800)
#define FRAME_HEIGHT (1280)
#define LCM_PHYSICAL_WIDTH_MM	(114)	// Screen width in MM
#define LCM_PHYSICAL_HEIGHT_MM	(173)	// Screen height in MM
#define LCM_PHYSICAL_WIDTH_UM	(LCM_PHYSICAL_WIDTH_MM * 1000)	// Screen width in UM
#define LCM_PHYSICAL_HEIGHT_UM	(LCM_PHYSICAL_HEIGHT_MM * 1000)	// Screen height in UM
#define REGFLAG_DELAY 0xFE
/* END OF REGISTERS MARKER */
#define REGFLAG_END_OF_TABLE 0xFC


#define PROJECT_ID_PROTO	0x2	/* Athena use abc123's display in ptoto build, proiect_id is 0x2 */
#define PROJECT_ID	0x3	/* Athena's own project_id is 0x3 */

#define LCM_ID_LOW_MASK   0x0F
#define LCM_ID_HIGH_MASK  0xF0

#define DGC_SEL_SHIFT 24
#define DGC_SEL_REG 0x2C1500

#define JD936X_KD_HSD		0x1 /* KD, using JD936X IC, abc123 HSD cell */
#define JD936X_KD_HSD_NEW	0x2 /* KD, using JD936X IC, ATHENA HSD cell */
#define JD936X_KD_HSD_PROTO	0x3 /* KD, using JD936X IC, abc123 HSD cell */
#define JD936X_BOE		0x4 /* BOE, using JD936X IC */
#define JD936X_BOE_NEW	0x5 /* BOE, using JD936X IC */
#define JD936X_INX		0x7 /* INX, using JD936X IC */
#define ST_UNKNOWN	0xFF

#define GPIO_OUT_ONE 1
#define GPIO_OUT_ZERO 0
#define GPIO_OUT_HIGH 1
#define GPIO_OUT_LOW 0
#define TRUE 1
#define FALSE 0

// ---------------------------------------------------------------------------
//  Local Variables
// ---------------------------------------------------------------------------

static LCM_UTIL_FUNCS lcm_util = {
	.set_reset_pin = NULL,
	.set_gpio_out = NULL,
	.udelay = NULL,
	.mdelay = NULL,
};

#define SET_RESET_PIN(v)      					(lcm_util.set_reset_pin((v)))
#define SET_GPIO_OUT(n, v)	        			(lcm_util.set_gpio_out((n), (v)))
#define UDELAY(n) 						(lcm_util.udelay(n))
#define MDELAY(n) 						(lcm_util.mdelay(n))

static unsigned char project_id;	/* Indentify Athena hvt lcm(KD_HSD+ILI9881C) vendor_id(0x3) and proto lcm(KD_HSD+JD936X)(0x3) */
static unsigned char vendor_id;
unsigned char lcm_id;
static unsigned char DGC_SEL;    /* JD6366's DGC_SEL register no otp function, read it from DDB1 regiser and save it when power on */

static struct regulator *lcm_vio;

static unsigned int GPIO_LCD_RST_EN;
static unsigned int GPIO_LCD_PWR_P_EN;
static unsigned int GPIO_LCD_PWR_N_EN;

extern int backlight_brightness_set(int level);
static void lcm_get_gpio_infor(struct device *dev);
static int lcm_get_vio_supply(struct device *dev);
static void lcm_get_DGC_SEL(void);

#define dsi_set_cmdq_V2(cmd, count, ppara, force_update)	lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define dsi_set_cmdq(pdata, queue_size, force_update)		lcm_util.dsi_set_cmdq(pdata, queue_size, force_update)
#define wrtie_cmd(cmd)						lcm_util.dsi_write_cmd(cmd)
#define write_regs(addr, pdata, byte_nums)			lcm_util.dsi_write_regs(addr, pdata, byte_nums)
#define read_reg(cmd)						lcm_util.dsi_dcs_read_lcm_reg(cmd)
#define read_reg_v2(cmd, buffer, buffer_size)   		lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, buffer_size)

#define   LCM_DSI_CMD_MODE	0
struct LCM_setting_table
{
	unsigned char cmd;
	unsigned char count;
	unsigned char para_list[64];
};

#ifdef BUILD_LK

#ifdef GPIO_LCM_RST
#define GPIO_LCD_RST_EN		GPIO_LCM_RST
#else
#define GPIO_LCD_RST_EN		(0xFFFFFFFF)
#endif
#endif

static int __init setup_lcm_id(char *str)
{
	int id;

	if (get_option(&str, &id))
	{
		lcm_id = (unsigned char)id;
	}
	return 0;
}
__setup("jd936x_id=", setup_lcm_id);

static void get_lcm_id(void)
{
	vendor_id = (unsigned int)(lcm_id & LCM_ID_LOW_MASK);
	project_id = (unsigned int)((lcm_id & LCM_ID_HIGH_MASK) >> 4);
	pr_debug("LCM: vendor_id = 0x%x,project_id = 0x%x\n",vendor_id,project_id);
}

static void lcm_get_gpio_infor(struct device *dev)
{
	pr_debug("LCM: lcm_get_gpio_infor is going\n");

	/* athena no external lcm-pmic, no need to asign en pin */
	GPIO_LCD_RST_EN = of_get_named_gpio(dev->of_node, "lcm_reset_gpio", 0);
	gpio_request(GPIO_LCD_RST_EN, "GPIO_LCD_RST_EN");
	pr_debug("LCM: GPIO_LCD_RST_EN=%d.\n",GPIO_LCD_RST_EN);

	GPIO_LCD_PWR_P_EN = of_get_named_gpio(dev->of_node, "lcm_power_p_gpio", 0);
	gpio_request(GPIO_LCD_PWR_P_EN, "GPIO_LCD_PWR_P_EN");
	pr_debug("LCM: GPIO_LCD_PWR_P_EN=%d.\n",GPIO_LCD_PWR_P_EN);

	GPIO_LCD_PWR_N_EN = of_get_named_gpio(dev->of_node, "lcm_power_n_gpio", 0);
	gpio_request(GPIO_LCD_PWR_N_EN, "GPIO_LCD_PWR_N_EN");
	pr_debug("LCM: GPIO_LCD_PWR_N_EN=%d.\n",GPIO_LCD_PWR_N_EN);
}

static void lcm_set_gpio_output(unsigned int GPIO, unsigned int output)
{
	gpio_direction_output(GPIO, output);
	gpio_set_value(GPIO, output);
}

static int lcm_get_vio_supply(struct device *dev)
{
	int ret = 0;
	struct regulator *lcm_vio_ldo;	/* athena 1V8, use mt6358-VCAMA2 */

	pr_debug("LCM: lcm_get_vio_supply is going\n");

	lcm_vio_ldo = devm_regulator_get(dev, "lcm-vio");
	if (IS_ERR(lcm_vio_ldo)) {
		ret = PTR_ERR(lcm_vio_ldo);
		dev_err(dev, "failed to get lcm-vio LDO, %d\n", ret);
		devm_regulator_put(lcm_vio_ldo);
		return ret;
	}

	lcm_vio = lcm_vio_ldo;
	pr_debug("LCM: lcm get lcm-vio supply ok.\n");

	return ret;
}

int lcm_vio_supply_enable(void)
{
	int ret = 0;
	unsigned int volt;

	pr_debug("LCM: Enter lcm_vio_supply_enable\n");

	if (NULL == lcm_vio) {
		pr_err("LCM: Oops, do nothing and leave lcm_vio_supply_enable\n");
		return 0;
	}

	pr_debug("LCM: set regulator voltage lcm_vio voltage to 1.8V\n");
	/* athena lcm_vio=1V8, use mt6358-VCAMA2 */
	ret = regulator_set_voltage(lcm_vio, 1800000, 1800000);

	if (ret != 0) {
		pr_err("LCM: lcm failed to set lcm_vio voltage: %d\n", ret);
		return ret;
	}

	/* get voltage settings again */
	volt = regulator_get_voltage(lcm_vio);
	if (volt == 1800000)
		pr_debug("LCM: check regulator voltage=1800000 pass!\n");
	else
		pr_err("LCM: check regulator voltage=1800000 fail! (voltage: %d)\n", volt);

	ret = regulator_enable(lcm_vio);
	if (ret != 0)
		pr_err("LCM: Failed to enable lcm_vio: %d\n", ret);

	return ret;
}

int lcm_vio_supply_disable(void)
{
	int ret = 0;
	unsigned int isenable;

	pr_debug("LCM: lcm_vio_supply_disable\n");

	if (NULL == lcm_vio) {
		pr_err("LCM: Oops, do nothing and leave lcm_vio_supply_disable\n");
		return 0;
	}

	pr_debug("LCM: close supply lcm_vio(1V8)\n");

	/* athena lcm_vio=1V8, use mt6358-VCAMA2 */
	isenable = regulator_is_enabled(lcm_vio);

	pr_debug("LCM: lcm_vio query regulator enable status[%d]\n", isenable);

	if (isenable) {
		ret = regulator_disable(lcm_vio);
		if (ret != 0) {
			pr_err("LCM: lcm failed to disable lcm_vio: %d\n", ret);
			return ret;
		}
		else {
			pr_debug("LCM: lcm disable lcm_vio\n");
		}

		/* verify */
		isenable = regulator_is_enabled(lcm_vio);
		if (!isenable)
			pr_debug("LCM: lcm_vio regulator disable pass\n");
		else
			pr_err("LCM: lcm_vio regulator disable fail\n");
	}

	return ret;
}

static bool is_fitipower_lcm_ic(void)
{
	get_lcm_id();
	/*Athena FITIPOWER lcm vendor id is 1, 2, 3, 4, 5, 7*/
	if ((vendor_id == JD936X_KD_HSD)||(vendor_id == JD936X_KD_HSD_NEW)||(vendor_id == JD936X_BOE)||(vendor_id == JD936X_BOE_NEW)||(vendor_id == JD936X_INX)||((project_id == PROJECT_ID_PROTO) && (vendor_id == JD936X_KD_HSD_PROTO))) {
		pr_debug("LCM: vendor_id is 0x%x\n",vendor_id);
		return true;
	} else {
		pr_err("LCM: there is no suitable vendor id for fitipower found\n");
		return false;
	}
}

static int lcm_driver_probe(struct device *dev, void const *data)
{
	pr_debug("LCM: lcm_driver_probe \n");

	lcm_get_vio_supply(dev);

	if (is_fitipower_lcm_ic()) {
		lcm_vio_supply_enable();
	}

	lcm_get_gpio_infor(dev);

	return 0;
}
static const struct of_device_id lcm_platform_of_match[] = {
	{
		.compatible = "lcm,lcm_dts_jd936x_athena",
		.data = 0,
	}, {
		/* sentinel */
	}
};


MODULE_DEVICE_TABLE(of, platform_of_match);

static int lcm_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;

	id = of_match_node(lcm_platform_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	return lcm_driver_probe(&pdev->dev, id->data);
}



static struct platform_driver lcm_driver = {
	.probe = lcm_platform_probe,
	.driver = {
			.name = "lcm_dts_jd936x_athena",
			.owner = THIS_MODULE,
			.of_match_table = lcm_platform_of_match,
			},
};

static int __init lcm_init(void)
{
	if (platform_driver_register(&lcm_driver)) {
		pr_err("LCM: failed to register disp driver\n");
		return -ENODEV;
	}

	return 0;
}

static void __exit lcm_exit(void)
{
	platform_driver_unregister(&lcm_driver);
	pr_err("LCM: Unregister lcm driver done\n");
}
late_initcall(lcm_init);
module_exit(lcm_exit);
MODULE_AUTHOR("mediatek");
MODULE_DESCRIPTION("Display subsystem Driver");
MODULE_LICENSE("GPL");

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_st_kd_hsd_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_MM;
	params->physical_height = LCM_PHYSICAL_HEIGHT_MM;
	params->physical_width_um = LCM_PHYSICAL_WIDTH_UM;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT_UM;

	/* SYNC_PULSE_VDO_MODE;BURST_VDO_MODE;SYNC_EVENT_VDO_MODE; */
	params->dsi.mode		     = BURST_VDO_MODE;
	/* Command mode setting */
	params->dsi.LANE_NUM		     = LCM_FOUR_LANE;
	//The following defined the fomat for data coming from LCD engine.
	params->dsi.data_format.color_order  = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq    = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding	     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format	     = LCM_DSI_FORMAT_RGB888;

	/* Highly depends on LCD driver capability. */
	params->dsi.packet_size = 256;
	// Video mode setting
	params->dsi.intermediat_buffer_num   = 0;
	params->dsi.PS			     = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count		     = FRAME_WIDTH*3;

	/* lcm optimize:reset voltage during print screen */
	params->dsi.cont_clock		     = 0;
	params->dsi.clk_lp_per_line_enable   = 1;

	params->dsi.vertical_sync_active     = 4;
	params->dsi.vertical_backporch	     = 12;
	params->dsi.vertical_frontporch	     = 20;
	params->dsi.vertical_active_line     = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active   = 24;
	params->dsi.horizontal_backporch     = 24;
	params->dsi.horizontal_frontporch    = 30;
	params->dsi.horizontal_active_pixel  = FRAME_WIDTH;

	/* PLL CLOCK 1316 X 878 X 24 X 60 % 4 X 1.15 % 2 = 239 */
	params->dsi.PLL_CLOCK	 = 239;
	params->dsi.ssc_disable	 = 0; /* 1:disable ssc , 0:enable ssc */
	params->dsi.HS_TRAIL = 5;
	params->dsi.CLK_HS_PRPR = 4;
}

static void lcm_get_st_kd_hsd_new_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_MM;
	params->physical_height = LCM_PHYSICAL_HEIGHT_MM;
	params->physical_width_um = LCM_PHYSICAL_WIDTH_UM;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT_UM;

	/* SYNC_PULSE_VDO_MODE;BURST_VDO_MODE;SYNC_EVENT_VDO_MODE; */
	params->dsi.mode		     = BURST_VDO_MODE;
	/* Command mode setting */
	params->dsi.LANE_NUM		     = LCM_FOUR_LANE;
	//The following defined the fomat for data coming from LCD engine.
	params->dsi.data_format.color_order  = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq    = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding	     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format	     = LCM_DSI_FORMAT_RGB888;

	/* Highly depends on LCD driver capability. */
	params->dsi.packet_size = 256;
	// Video mode setting
	params->dsi.intermediat_buffer_num   = 0;
	params->dsi.PS			     = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count		     = FRAME_WIDTH*3;

	/* lcm optimize:reset voltage during print screen */
	params->dsi.cont_clock		     = 0;
	params->dsi.clk_lp_per_line_enable   = 1;

	params->dsi.vertical_sync_active     = 4;
	params->dsi.vertical_backporch	     = 12;
	params->dsi.vertical_frontporch	     = 20;
	params->dsi.vertical_active_line     = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active   = 24;
	params->dsi.horizontal_backporch     = 24;
	params->dsi.horizontal_frontporch    = 30;
	params->dsi.horizontal_active_pixel  = FRAME_WIDTH;

	/* PLL CLOCK 1316 X 878 X 24 X 60 % 4 X 1.15 % 2 = 239 */
	params->dsi.PLL_CLOCK	 = 239;
	params->dsi.ssc_disable	 = 0; /* 1:disable ssc , 0:enable ssc */
	params->dsi.HS_TRAIL = 5;
	params->dsi.CLK_HS_PRPR = 4;
}

static void lcm_get_st_boe_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_MM;
	params->physical_height = LCM_PHYSICAL_HEIGHT_MM;
	params->physical_width_um = LCM_PHYSICAL_WIDTH_UM;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT_UM;

	/* SYNC_PULSE_VDO_MODE;BURST_VDO_MODE;SYNC_EVENT_VDO_MODE; */
	params->dsi.mode		     = BURST_VDO_MODE;
	/* Command mode setting */
	params->dsi.LANE_NUM		     = LCM_FOUR_LANE;
	/* The following defined the fomat for data coming from LCD engine. */
	params->dsi.data_format.color_order  = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq    = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding	     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format	     = LCM_DSI_FORMAT_RGB888;

	/* Highly depends on LCD driver capability. */
	params->dsi.packet_size = 256;

	/* Video mode setting */
	params->dsi.intermediat_buffer_num   = 0;
	params->dsi.PS			     = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count		     = FRAME_WIDTH*3;

	/* lcm optimize:reset voltage during print screen */
	params->dsi.cont_clock		     = 0;
	params->dsi.clk_lp_per_line_enable   = 1;

	params->dsi.vertical_sync_active     = 4;
	params->dsi.vertical_backporch	     = 4;
	params->dsi.vertical_frontporch	     = 8;
	params->dsi.vertical_active_line     = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active   = 20;
	params->dsi.horizontal_backporch     = 42;
	params->dsi.horizontal_frontporch    = 32;
	params->dsi.horizontal_active_pixel  = FRAME_WIDTH;

	/* PLL_CLOCK: 1296 X 872 X 24 X 60 % 4 X 1.15 % 2 = 234 */
	params->dsi.PLL_CLOCK	 = 239;
	params->dsi.ssc_disable	 = 0; /* 1:disable ssc , 0:enable ssc */
	params->dsi.HS_TRAIL = 5;
	params->dsi.CLK_HS_PRPR = 4;
}

static void lcm_get_st_boe_new_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_MM;
	params->physical_height = LCM_PHYSICAL_HEIGHT_MM;
	params->physical_width_um = LCM_PHYSICAL_WIDTH_UM;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT_UM;

	/* SYNC_PULSE_VDO_MODE;BURST_VDO_MODE;SYNC_EVENT_VDO_MODE; */
	params->dsi.mode		     = BURST_VDO_MODE;
	/* Command mode setting */
	params->dsi.LANE_NUM		     = LCM_FOUR_LANE;
	/* The following defined the fomat for data coming from LCD engine. */
	params->dsi.data_format.color_order  = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq    = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding	     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format	     = LCM_DSI_FORMAT_RGB888;

	/* Highly depends on LCD driver capability. */
	params->dsi.packet_size = 256;

	/* Video mode setting */
	params->dsi.intermediat_buffer_num   = 0;
	params->dsi.PS			     = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count		     = FRAME_WIDTH*3;

	/* lcm optimize:reset voltage during print screen */
	params->dsi.cont_clock		     = 0;
	params->dsi.clk_lp_per_line_enable   = 1;

	params->dsi.vertical_sync_active     = 4;
	params->dsi.vertical_backporch	     = 12;
	params->dsi.vertical_frontporch	     = 30;
	params->dsi.vertical_active_line     = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active   = 20;
	params->dsi.horizontal_backporch     = 20;
	params->dsi.horizontal_frontporch    = 30;
	params->dsi.horizontal_active_pixel  = FRAME_WIDTH;

	/* PLL_CLOCK: 1326 X 880 X 24 X 60 % 4 X 1.15 % 2 = 241 */
	params->dsi.PLL_CLOCK	 = 239;
	params->dsi.ssc_disable	 = 0; /* 1:disable ssc , 0:enable ssc */
	params->dsi.HS_TRAIL = 5;
	params->dsi.CLK_HS_PRPR = 4;
}

static void lcm_get_st_inx_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->physical_width = LCM_PHYSICAL_WIDTH_MM;
	params->physical_height = LCM_PHYSICAL_HEIGHT_MM;
	params->physical_width_um = LCM_PHYSICAL_WIDTH_UM;
	params->physical_height_um = LCM_PHYSICAL_HEIGHT_UM;

	/* SYNC_PULSE_VDO_MODE;BURST_VDO_MODE;SYNC_EVENT_VDO_MODE; */
	params->dsi.mode		     = BURST_VDO_MODE;
	/* Command mode setting */
	params->dsi.LANE_NUM		     = LCM_FOUR_LANE;
	/* The following defined the fomat for data coming from LCD engine. */
	params->dsi.data_format.color_order  = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq    = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding	     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format	     = LCM_DSI_FORMAT_RGB888;

	/* Highly depends on LCD driver capability. */
	params->dsi.packet_size = 256;

	/* Video mode setting */
	params->dsi.intermediat_buffer_num   = 0;
	params->dsi.PS			     = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count		     = FRAME_WIDTH*3;

	/* lcm optimize:reset voltage during print screen */
	params->dsi.cont_clock		     = 0;
	params->dsi.clk_lp_per_line_enable   = 1;

	params->dsi.vertical_sync_active     = 4;
	params->dsi.vertical_backporch	     = 12;
	params->dsi.vertical_frontporch	     = 30;
	params->dsi.vertical_active_line     = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active   = 20;
	params->dsi.horizontal_backporch     = 20;
	params->dsi.horizontal_frontporch    = 30;
	params->dsi.horizontal_active_pixel  = FRAME_WIDTH;

	/* PLL_CLOCK: 1326 X 880 X 24 X 60 % 4 X 1.15 % 2 = 241 */
	params->dsi.PLL_CLOCK	 = 239;
	params->dsi.ssc_disable	 = 0; /* 1:disable ssc , 0:enable ssc */
	params->dsi.HS_TRAIL = 5;
	params->dsi.CLK_HS_PRPR = 4;
}

static void lcm_reset(void)
{
	pr_info("[KERNEL/LCM] %s, - Reset\n", __func__);

	lcm_set_gpio_output(GPIO_LCD_RST_EN,GPIO_OUT_HIGH);
	UDELAY(50);
	lcm_set_gpio_output(GPIO_LCD_RST_EN,GPIO_OUT_LOW);
	UDELAY(50);
	lcm_set_gpio_output(GPIO_LCD_RST_EN,GPIO_OUT_HIGH);
	MDELAY(50);
}

static char *lcm_get_vendor_type(void)
{
	if (vendor_id == JD936X_KD_HSD)
		return "JD936X_KD_HSD\0";
	else if (vendor_id == JD936X_KD_HSD_NEW)
		return "JD936X_KD_HSD_NEW\0";
	else if ((project_id == PROJECT_ID_PROTO) && (vendor_id == JD936X_KD_HSD_PROTO))
		return "JD936X_KD_HSD_PROTO\0";
	else if (vendor_id == JD936X_BOE)
		return "JD936X_BOE\0";
	else if (vendor_id == JD936X_BOE_NEW)
		return "JD936X_BOE_NEW\0";
	else if (vendor_id == JD936X_INX)
		return "JD936X_INX\0";
	else
		return "UNKNOWN\0";
}

static void init_athena_jd936x_kd_hsd_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x93E11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65E21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF8E31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x32041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xEF181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xEF1B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFE241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x123A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x783C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA0411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x14431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x30451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x044B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x69571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x285A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x195B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7A5D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x645E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x555F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x48601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x36621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3A631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3B651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x39661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x56681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x43691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4C6A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3F6B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3E6C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x346D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x286E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x106F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7A701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x64711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x55721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x48731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x36751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3A761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3B781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x39791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x387A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x567B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x437C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4C7D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3F7E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3E7F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x34801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5E001500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x77031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x48041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x420F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5E161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x77191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x491A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B1B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x451C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x471D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x411E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F1F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x43251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F2A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F2B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1E2D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x172E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x172F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x013B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1E431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06461500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04471500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A481500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08491500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x024A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x305B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x055C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x305D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x025F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x30601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6A641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x75651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x73671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6A6A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x086B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x046E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x886F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7B731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x037D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7B7E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(5);
}

static void init_athena_jd936x_kd_hsd_new_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x93E11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65E21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF8E31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2F041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x101A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F1B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x011C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFE241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x123A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x783C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA0411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x14431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x30451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x044B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x285A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F5B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7C5D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x685E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x585F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x35621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x35661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x34671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4E681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3D6A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2E6B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2B6C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x206D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x136E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7C701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x68711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x58721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x35751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x35791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x347A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4E7B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x387C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3D7D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2E7E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2B7F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x20801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x13811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5E001500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x58031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x48061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x500F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5E161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x58191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x451A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x471B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x491C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B1D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x411E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F1F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x51251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F2A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F2B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1E2D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x172E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x182F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x11341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x013B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1E431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x18451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A461500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08471500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06481500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04491500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x104A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x305B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B5C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x305D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x025F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x30601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1C631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6A641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x75651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x73671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1C691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6A6A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x886F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7B731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xBB751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x24781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x037D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7B7E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(5);
}

static void init_athena_jd936x_boe_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	/* read DDB1 in page0 */
	lcm_get_DGC_SEL();

	data_array[0] = 0x93E11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65E21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF8E31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xB1181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xB11B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x011C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3E1F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x19371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x123A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x783C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA0411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x044B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x285A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x145B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7C5D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x665E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x575F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x25641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3E651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3B661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3A671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x55681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x476A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x386B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x316C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x226D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x116E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7C701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x66711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x25771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3E781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3B791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3A7A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x557B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x407C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x477D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x387E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x317F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x22801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x11811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x47001500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x47011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x41081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x430F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A1A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A1B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x481C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x481D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x401E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F1F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x42251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x112C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F2D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D2E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B2F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x18331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x133F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08461500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06471500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04481500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x18491500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x174A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x004C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x305B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x025C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x405D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x025F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x62631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x62641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x74671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x62691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x666A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x086B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x086F1500;
	dsi_set_cmdq(data_array, 1, 1);

    /* Page3 enable DGC function */
	data_array[0] = 0x03E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x012B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = (DGC_SEL << DGC_SEL_SHIFT | DGC_SEL_REG);
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x480E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2B2B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x442E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02E61500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0CE71500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(5);
}

static void init_athena_jd936x_boe_new_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x93E11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65E21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF8E31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xB7181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xB71B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFE241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x19371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x013A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x013B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x703C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA0411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1E431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x044B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA9571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x375A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x195B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x785D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x645E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x565F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x48611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3A621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2B641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x63681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x51691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x586A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x496B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x446C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x366D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x226E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x106F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x78701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x64711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x56721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x48741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3A751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2B771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x457A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x637B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x517C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x587D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x497E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x447F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x36801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x22811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x47001500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x47011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x41081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x77121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x35131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x46171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x44191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A1A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A1B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x481C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x481D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x401E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F1F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x77281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x35291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x105B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x065C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x405D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x015E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x025F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x30601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6B641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x73671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x566A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x086B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x046D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x046E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x886F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7B731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF8751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xD5771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2E781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x037A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x007C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x037D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7B7E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x019A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x049B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01AF1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x11091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2A0E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x59361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x80511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2C531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(5);

	data_array[0] = 0x00351500;
	dsi_set_cmdq(data_array, 1, 1);
}

static void init_athena_jd936x_inx_lcm(void)
{
	unsigned int data_array[64];

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x93E11500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x65E21500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF8E31500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x01E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF7181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xF71B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x001C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFE241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x013A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x703C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xFF3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7F3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x06401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xA0411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1E431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x28451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x69571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A591500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2E5A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x195B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x155C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7A5D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6B5E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F5F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x54601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x53611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4C631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6E681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x59691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5E6A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4E6B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x496C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C6D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2A6E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x106F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x7A701500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6B711500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F721500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x54731500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x53741500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4C761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x38771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52791500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x527A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6E7B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x597C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5E7D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4E7E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x497F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x3C801500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2A811500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10821500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x02E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x50001500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F011500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x52031500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x77041500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x57051500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F061500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4E071500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4C081500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4A0A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x480B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F0C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x460D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x440E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x400F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F101500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F111500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F121500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F131500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F141500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F151500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x51161500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F171500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F181500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x53191500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x771A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x571B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F1C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4F1D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4D1E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F1F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x4B201500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49211500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F221500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x47231500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x45241500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x41251500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F261500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F271500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F281500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F291500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F2A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x5F2B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x012C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F2E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x132F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17301500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17311500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F321500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0D331500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0F341500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F351500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x07371500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F381500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x09391500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0B3A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x113B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F3F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F401500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F411500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00421500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F431500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F441500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x12451500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17461500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x17471500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F481500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C491500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0E4A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x044C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x064D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F4E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x084F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0A501500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x10511500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F521500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F531500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F541500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F551500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F561500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x1F571500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40581500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x105B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x065C1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x405D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x005F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x40601500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x03611500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04621500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6C631500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6C641500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x75651500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08661500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xB4671500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x08681500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6C691500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x6C6A1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x0C6B1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006D1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x006E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x886F1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0xBB751500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00761500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x05771500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x2A781500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x04E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x23021500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x11091500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x480E1500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x49361500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00E01500;
	dsi_set_cmdq(data_array, 1, 1);

	data_array[0] = 0x00110500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);

	data_array[0] = 0x00290500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(5);
}

static void lcm_init_lcm(void)
{
	get_lcm_id();

	if (vendor_id == JD936X_KD_HSD)
		init_athena_jd936x_kd_hsd_lcm();    /* JD936X KD panel, HSD glass */
	else if (vendor_id == JD936X_KD_HSD_NEW)
		init_athena_jd936x_kd_hsd_new_lcm();
	else if ((project_id == PROJECT_ID_PROTO) && (vendor_id == JD936X_KD_HSD_PROTO))
		init_athena_jd936x_kd_hsd_lcm();	/* JD936X KD panel, HSD glass */
	else if (vendor_id == JD936X_BOE)
		init_athena_jd936x_boe_lcm();	/*JD936X BOE panel */
	else if (vendor_id == JD936X_BOE_NEW)
		init_athena_jd936x_boe_new_lcm();
	else if (vendor_id == JD936X_INX)
		init_athena_jd936x_inx_lcm();	/*JD936X INX panel */
	else
		init_athena_jd936x_kd_hsd_lcm();	    /* Default JD936X KD_HSD panel */

	pr_debug("[JD936X_lcm]lcm_init_lcm func:Kernel athena jd936x %s lcm init ok!\n",lcm_get_vendor_type());
}

static void lcm_suspend(void)
{
	unsigned int data_array[16];

	data_array[0] = 0x00280500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(20);

	data_array[0] = 0x00100500;
	dsi_set_cmdq(data_array, 1, 1);
	MDELAY(120);
}

static void lcm_suspend_power(void)
{
	pr_debug("LCM: lcm try to turn off supply\n");

	lcm_set_gpio_output(GPIO_LCD_RST_EN,GPIO_OUT_LOW);
	MDELAY(120);
	lcm_set_gpio_output(GPIO_LCD_PWR_N_EN,GPIO_OUT_LOW);
	MDELAY(10);
	lcm_set_gpio_output(GPIO_LCD_PWR_P_EN,GPIO_OUT_LOW);
	MDELAY(10);

	if (lcm_vio_supply_disable()) {
		pr_err(KERN_ERR "<%s>JD936X lcm(lcm_vio) failed to turn off supply", __func__);
	}
}

static void lcm_resume_power(void)
{
	pr_debug("LCM: lcm try to turn on supply\n");

	if(lcm_vio_supply_enable())
		pr_err(KERN_ERR "<%s>JD936X lcm(lcm_vio) failed to turn on supply", __func__);
	MDELAY(10);
	lcm_set_gpio_output(GPIO_LCD_PWR_P_EN,GPIO_OUT_HIGH);
	MDELAY(10);
	lcm_set_gpio_output(GPIO_LCD_PWR_N_EN,GPIO_OUT_HIGH);
	MDELAY(10);
}

static void lcm_get_DGC_SEL(void)
{
	read_reg_v2(0xA1, &DGC_SEL, 1);
	pr_debug("[LK/LCM][JD9366BA] %s, DGC_SEL = 0x%x\n",__func__, DGC_SEL);
}

static void lcm_resume(void)
{
	get_lcm_id();

	MDELAY(20);
	lcm_reset();

	if (vendor_id == JD936X_KD_HSD)
		init_athena_jd936x_kd_hsd_lcm();    /* JD936X KD panel, HSD glass */
	else if (vendor_id == JD936X_KD_HSD_NEW)
		init_athena_jd936x_kd_hsd_new_lcm();
	else if ((project_id == PROJECT_ID_PROTO) && (vendor_id == JD936X_KD_HSD_PROTO))
		init_athena_jd936x_kd_hsd_lcm();	/* JD936X KD panel, HSD glass */
	else if (vendor_id == JD936X_BOE)
		init_athena_jd936x_boe_lcm();	/*JD936X BOE panel */
	else if (vendor_id == JD936X_BOE_NEW)
		init_athena_jd936x_boe_new_lcm();
	else if (vendor_id == JD936X_INX)
		init_athena_jd936x_inx_lcm();	/*JD936X INX panel */
	else
		init_athena_jd936x_kd_hsd_lcm();	    /* Default JD936X KD_HSD panel */
}

static void lcm_set_backlight(unsigned int level)
{
	backlight_brightness_set(level);
}

LCM_DRIVER jd936x_wxga_dsi_vdo_athena_st_kd_hsd_lcm_drv = {
	.name		 = "jd936x_wxga_dsi_vdo_athena_st_kd_hsd",
	.set_util_funcs	 = lcm_set_util_funcs,
	.get_params	 = lcm_get_st_kd_hsd_params,
	.init		 = lcm_init_lcm,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight  = lcm_set_backlight,
};

LCM_DRIVER jd936x_wxga_dsi_vdo_athena_st_kd_hsd_new_lcm_drv = {
	.name		 = "jd936x_wxga_dsi_vdo_athena_st_kd_hsd_new",
	.set_util_funcs	 = lcm_set_util_funcs,
	.get_params	 = lcm_get_st_kd_hsd_new_params,
	.init		 = lcm_init_lcm,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight  = lcm_set_backlight,
};

LCM_DRIVER jd936x_wxga_dsi_vdo_athena_st_kd_hsd_proto_lcm_drv = {
	.name		 = "jd936x_wxga_dsi_vdo_athena_st_kd_hsd_proto",
	.set_util_funcs	 = lcm_set_util_funcs,
	.get_params	 = lcm_get_st_kd_hsd_params,
	.init		 = lcm_init_lcm,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight  = lcm_set_backlight,
};

LCM_DRIVER jd936x_wxga_dsi_vdo_athena_st_boe_lcm_drv = {
	.name		 = "jd936x_wxga_dsi_vdo_athena_st_boe",
	.set_util_funcs	 = lcm_set_util_funcs,
	.get_params	 = lcm_get_st_boe_params,
	.init		 = lcm_init_lcm,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight  = lcm_set_backlight,
};

LCM_DRIVER jd936x_wxga_dsi_vdo_athena_st_boe_new_lcm_drv = {
	.name		 = "jd936x_wxga_dsi_vdo_athena_st_boe_new",
	.set_util_funcs	 = lcm_set_util_funcs,
	.get_params	 = lcm_get_st_boe_new_params,
	.init		 = lcm_init_lcm,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight  = lcm_set_backlight,
};

LCM_DRIVER jd936x_wxga_dsi_vdo_athena_st_inx_lcm_drv = {
	.name		 = "jd936x_wxga_dsi_vdo_athena_st_inx",
	.set_util_funcs	 = lcm_set_util_funcs,
	.get_params	 = lcm_get_st_inx_params,
	.init		 = lcm_init_lcm,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.resume_power	= lcm_resume_power,
	.suspend_power	= lcm_suspend_power,
	.set_backlight  = lcm_set_backlight,
};
