/*
 * arch/arm/mach-tegra/board-dalmore-panel.c
 *
 * Copyright (c) 2011-2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <linux/ioport.h>
#include <linux/fb.h>
#include <linux/nvmap.h>
#include <linux/nvhost.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/tegra_pwm_bl.h>
#include <linux/regulator/consumer.h>
#include <linux/pwm_backlight.h>

#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/dc.h>

#include "board.h"
#include "devices.h"
#include "gpio-names.h"
#include "tegra11_host1x_devices.h"

struct platform_device * __init dalmore_host1x_init(void)
{
	struct platform_device *pdev = NULL;

#ifdef CONFIG_TEGRA_GRHOST
	pdev = tegra11_register_host1x_devices();
	if (!pdev) {
		pr_err("host1x devices registration failed\n");
		return NULL;
	}
#endif
	return pdev;
}

#ifdef CONFIG_TEGRA_DC

#define IS_EXTERNAL_PWM		1

/* PANEL_<diagonal length in inches>_<vendor name>_<resolution> */
#define PANEL_10_1_PANASONIC_1920_1200	1
#define PANEL_11_6_AUO_1920_1080	0
#define PANEL_10_1_SHARP_2560_1600	0

#if PANEL_10_1_SHARP_2560_1600
#define TEGRA_DSI_GANGED_MODE	1
#else
#define TEGRA_DSI_GANGED_MODE	0
#endif

#define DSI_PANEL_RESET		1
#define DSI_PANEL_RST_GPIO	TEGRA_GPIO_PH3
#define DSI_PANEL_BL_EN_GPIO	TEGRA_GPIO_PH2
#define DSI_PANEL_BL_PWM	TEGRA_GPIO_PH1

#define DC_CTRL_MODE	TEGRA_DC_OUT_CONTINUOUS_MODE

/* HDMI Hotplug detection pin */
#define dalmore_hdmi_hpd	TEGRA_GPIO_PN7

static atomic_t sd_brightness = ATOMIC_INIT(255);

static bool reg_requested;
static bool gpio_requested;

/* for PANEL_10_1_PANASONIC_1920_1200, PANEL_11_6_AUO_1920_1080
 * and PANEL_10_1_SHARP_2560_1600
 */
static struct regulator *avdd_lcd_3v3;
static struct regulator *vdd_lcd_bl_12v;

/* for PANEL_11_6_AUO_1920_1080 and PANEL_10_1_SHARP_2560_1600 */
static struct regulator *dvdd_lcd_1v8;

/* for PANEL_11_6_AUO_1920_1080 */
static struct regulator *vdd_ds_1v8;
#define en_vdd_bl	TEGRA_GPIO_PG0
#define lvds_en		TEGRA_GPIO_PG3

static struct regulator *dalmore_hdmi_reg;
static struct regulator *dalmore_hdmi_pll;
static struct regulator *dalmore_hdmi_vddio;

static struct resource dalmore_disp1_resources[] = {
	{
		.name	= "irq",
		.start	= INT_DISPLAY_GENERAL,
		.end	= INT_DISPLAY_GENERAL,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "regs",
		.start	= TEGRA_DISPLAY_BASE,
		.end	= TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.start	= 0, /* Filled in by dalmore_panel_init() */
		.end	= 0, /* Filled in by dalmore_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
#if TEGRA_DSI_GANGED_MODE
	{
		.name	= "ganged_dsia_regs",
		.start	= TEGRA_DSI_BASE,
		.end	= TEGRA_DSI_BASE + TEGRA_DSI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "ganged_dsib_regs",
		.start	= TEGRA_DSIB_BASE,
		.end	= TEGRA_DSIB_BASE + TEGRA_DSIB_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
#else
	{
		.name	= "dsi_regs",
		.start	= TEGRA_DSI_BASE,
		.end	= TEGRA_DSI_BASE + TEGRA_DSI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
#endif
	{
		.name	= "mipi_cal",
		.start	= TEGRA_MIPI_CAL_BASE,
		.end	= TEGRA_MIPI_CAL_BASE + TEGRA_MIPI_CAL_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource dalmore_disp2_resources[] = {
	{
		.name	= "irq",
		.start	= INT_DISPLAY_B_GENERAL,
		.end	= INT_DISPLAY_B_GENERAL,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "regs",
		.start	= TEGRA_DISPLAY2_BASE,
		.end	= TEGRA_DISPLAY2_BASE + TEGRA_DISPLAY2_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.start	= 0, /* Filled in by dalmore_panel_init() */
		.end	= 0, /* Filled in by dalmore_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "hdmi_regs",
		.start	= TEGRA_HDMI_BASE,
		.end	= TEGRA_HDMI_BASE + TEGRA_HDMI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

#if PANEL_10_1_PANASONIC_1920_1200
static tegra_dc_bl_output dalmore_bl_output_measured = {
	0, 0, 1, 2, 3, 4, 5, 6,
	7, 8, 9, 9, 10, 11, 12, 13,
	13, 14, 15, 16, 17, 17, 18, 19,
	20, 21, 22, 22, 23, 24, 25, 26,
	27, 27, 28, 29, 30, 31, 32, 32,
	33, 34, 35, 36, 37, 37, 38, 39,
	40, 41, 42, 42, 43, 44, 45, 46,
	47, 48, 48, 49, 50, 51, 52, 53,
	54, 55, 56, 57, 57, 58, 59, 60,
	61, 62, 63, 64, 65, 66, 67, 68,
	69, 70, 71, 71, 72, 73, 74, 75,
	76, 77, 77, 78, 79, 80, 81, 82,
	83, 84, 85, 87, 88, 89, 90, 91,
	92, 93, 94, 95, 96, 97, 98, 99,
	100, 101, 102, 103, 104, 105, 106, 107,
	108, 109, 110, 111, 112, 113, 115, 116,
	117, 118, 119, 120, 121, 122, 123, 124,
	125, 126, 127, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 139, 141,
	142, 143, 144, 146, 147, 148, 149, 151,
	152, 153, 154, 155, 156, 157, 158, 158,
	159, 160, 161, 162, 163, 165, 166, 167,
	168, 169, 170, 171, 172, 173, 174, 176,
	177, 178, 179, 180, 182, 183, 184, 185,
	186, 187, 188, 189, 190, 191, 192, 194,
	195, 196, 197, 198, 199, 200, 201, 202,
	203, 204, 205, 206, 207, 208, 209, 210,
	211, 212, 213, 214, 215, 216, 217, 219,
	220, 221, 222, 224, 225, 226, 227, 229,
	230, 231, 232, 233, 234, 235, 236, 238,
	239, 240, 241, 242, 243, 244, 245, 246,
	247, 248, 249, 250, 251, 252, 253, 255
};
#elif PANEL_11_6_AUO_1920_1080
/* TODO: Calibrate for AUO panel */
static tegra_dc_bl_output dalmore_bl_output_measured = {
	0, 0, 1, 2, 3, 4, 5, 6,
	7, 8, 9, 9, 10, 11, 12, 13,
	13, 14, 15, 16, 17, 17, 18, 19,
	20, 21, 22, 22, 23, 24, 25, 26,
	27, 27, 28, 29, 30, 31, 32, 32,
	33, 34, 35, 36, 37, 37, 38, 39,
	40, 41, 42, 42, 43, 44, 45, 46,
	47, 48, 48, 49, 50, 51, 52, 53,
	54, 55, 56, 57, 57, 58, 59, 60,
	61, 62, 63, 64, 65, 66, 67, 68,
	69, 70, 71, 71, 72, 73, 74, 75,
	76, 77, 77, 78, 79, 80, 81, 82,
	83, 84, 85, 87, 88, 89, 90, 91,
	92, 93, 94, 95, 96, 97, 98, 99,
	100, 101, 102, 103, 104, 105, 106, 107,
	108, 109, 110, 111, 112, 113, 115, 116,
	117, 118, 119, 120, 121, 122, 123, 124,
	125, 126, 127, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 139, 141,
	142, 143, 144, 146, 147, 148, 149, 151,
	152, 153, 154, 155, 156, 157, 158, 158,
	159, 160, 161, 162, 163, 165, 166, 167,
	168, 169, 170, 171, 172, 173, 174, 176,
	177, 178, 179, 180, 182, 183, 184, 185,
	186, 187, 188, 189, 190, 191, 192, 194,
	195, 196, 197, 198, 199, 200, 201, 202,
	203, 204, 205, 206, 207, 208, 209, 210,
	211, 212, 213, 214, 215, 216, 217, 219,
	220, 221, 222, 224, 225, 226, 227, 229,
	230, 231, 232, 233, 234, 235, 236, 238,
	239, 240, 241, 242, 243, 244, 245, 246,
	247, 248, 249, 250, 251, 252, 253, 255
};
#elif PANEL_10_1_SHARP_2560_1600
/* TODO: Calibrate for SHARP panel */
static tegra_dc_bl_output dalmore_bl_output_measured = {
	0, 0, 1, 2, 3, 4, 5, 6,
	7, 8, 9, 9, 10, 11, 12, 13,
	13, 14, 15, 16, 17, 17, 18, 19,
	20, 21, 22, 22, 23, 24, 25, 26,
	27, 27, 28, 29, 30, 31, 32, 32,
	33, 34, 35, 36, 37, 37, 38, 39,
	40, 41, 42, 42, 43, 44, 45, 46,
	47, 48, 48, 49, 50, 51, 52, 53,
	54, 55, 56, 57, 57, 58, 59, 60,
	61, 62, 63, 64, 65, 66, 67, 68,
	69, 70, 71, 71, 72, 73, 74, 75,
	76, 77, 77, 78, 79, 80, 81, 82,
	83, 84, 85, 87, 88, 89, 90, 91,
	92, 93, 94, 95, 96, 97, 98, 99,
	100, 101, 102, 103, 104, 105, 106, 107,
	108, 109, 110, 111, 112, 113, 115, 116,
	117, 118, 119, 120, 121, 122, 123, 124,
	125, 126, 127, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 139, 141,
	142, 143, 144, 146, 147, 148, 149, 151,
	152, 153, 154, 155, 156, 157, 158, 158,
	159, 160, 161, 162, 163, 165, 166, 167,
	168, 169, 170, 171, 172, 173, 174, 176,
	177, 178, 179, 180, 182, 183, 184, 185,
	186, 187, 188, 189, 190, 191, 192, 194,
	195, 196, 197, 198, 199, 200, 201, 202,
	203, 204, 205, 206, 207, 208, 209, 210,
	211, 212, 213, 214, 215, 216, 217, 219,
	220, 221, 222, 224, 225, 226, 227, 229,
	230, 231, 232, 233, 234, 235, 236, 238,
	239, 240, 241, 242, 243, 244, 245, 246,
	247, 248, 249, 250, 251, 252, 253, 255
};
#endif

static p_tegra_dc_bl_output bl_output = dalmore_bl_output_measured;

static struct tegra_dsi_cmd dsi_init_cmd[] = {
#if PANEL_10_1_PANASONIC_1920_1200
	/* no init command required */
#endif
#if PANEL_11_6_AUO_1920_1080
	/* no init command required */
#endif
#if PANEL_10_1_SHARP_2560_1600
	DSI_CMD_SHORT(DSI_DCS_WRITE_0_PARAM, DSI_DCS_EXIT_SLEEP_MODE, 0x0),
	DSI_DLY_MS(2000),
	DSI_CMD_SHORT(DSI_DCS_WRITE_0_PARAM, DSI_DCS_SET_DISPLAY_ON, 0x0),
	DSI_DLY_MS(100),
#endif
};

const u32 __maybe_unused panasonic_1920_1200_vnb_syne[NUMOF_PKT_SEQ] = {
	PKT_ID0(CMD_VS) | PKT_LEN0(0) | PKT_ID1(CMD_BLNK) | PKT_LEN1(1) |
	PKT_ID2(CMD_HE) | PKT_LEN2(0) | PKT_LP,
	0,
	PKT_ID0(CMD_VE) | PKT_LEN0(0) | PKT_ID1(CMD_BLNK) | PKT_LEN1(1) |
	PKT_ID2(CMD_HE) | PKT_LEN2(0) | PKT_LP,
	0,
	PKT_ID0(CMD_HS) | PKT_LEN0(0) | PKT_ID1(CMD_BLNK) | PKT_LEN1(1) |
	PKT_ID2(CMD_HE) | PKT_LEN2(0) | PKT_LP,
	0,
	PKT_ID0(CMD_HS) | PKT_LEN0(0) | PKT_ID1(CMD_BLNK) | PKT_LEN1(1) |
	PKT_ID2(CMD_HE) | PKT_LEN2(0),
	PKT_ID3(CMD_BLNK) | PKT_LEN3(2) | PKT_ID4(CMD_RGB_24BPP) | PKT_LEN4(3) |
	PKT_ID5(CMD_BLNK) | PKT_LEN5(4),
	PKT_ID0(CMD_HS) | PKT_LEN0(0) | PKT_ID1(CMD_BLNK) | PKT_LEN1(1) |
	PKT_ID2(CMD_HE) | PKT_LEN2(0) | PKT_LP,
	0,
	PKT_ID0(CMD_HS) | PKT_LEN0(0) | PKT_ID1(CMD_BLNK) | PKT_LEN1(1) |
	PKT_ID2(CMD_HE) | PKT_LEN2(0),
	PKT_ID3(CMD_BLNK) | PKT_LEN3(2) | PKT_ID4(CMD_RGB_24BPP) | PKT_LEN4(3) |
	PKT_ID5(CMD_BLNK) | PKT_LEN5(4),
};

static struct tegra_dsi_out dalmore_dsi = {
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
	.n_data_lanes = 2,
	.controller_vs = DSI_VS_0,
#else
	.controller_vs = DSI_VS_1,
#endif
#if PANEL_11_6_AUO_1920_1080
	.dsi2edp_bridge_enable = true,
#endif
#if PANEL_10_1_SHARP_2560_1600
	.n_data_lanes = 8,
	.video_burst_mode = TEGRA_DSI_VIDEO_NONE_BURST_MODE,
	.ganged_type = TEGRA_DSI_GANGED_SYMMETRIC_EVEN_ODD,
#else
	.n_data_lanes = 4,
	.video_burst_mode = TEGRA_DSI_VIDEO_NONE_BURST_MODE_WITH_SYNC_END,
#endif
	.pixel_format = TEGRA_DSI_PIXEL_FORMAT_24BIT_P,
	.refresh_rate = 60,
	.virtual_channel = TEGRA_DSI_VIRTUAL_CHANNEL_0,

	.dsi_instance = DSI_INSTANCE_0,

	.panel_reset = DSI_PANEL_RESET,
	.power_saving_suspend = true,
#ifdef CONFIG_ARCH_TEGRA_3x_SOC
	.video_data_type = TEGRA_DSI_VIDEO_TYPE_COMMAND_MODE,
#else
	.video_data_type = TEGRA_DSI_VIDEO_TYPE_VIDEO_MODE,
	.video_clock_mode = TEGRA_DSI_VIDEO_CLOCK_TX_ONLY,
#endif
	.dsi_init_cmd = dsi_init_cmd,
	.n_init_cmd = ARRAY_SIZE(dsi_init_cmd),
#if PANEL_10_1_PANASONIC_1920_1200
	.pkt_seq = panasonic_1920_1200_vnb_syne,
#endif
};

static int dalmore_dsi_regulator_get(struct device *dev)
{
	int err = 0;

	if (reg_requested)
		return 0;

#if PANEL_11_6_AUO_1920_1080 || \
	PANEL_10_1_SHARP_2560_1600
	dvdd_lcd_1v8 = regulator_get(dev, "dvdd_lcd");
	if (IS_ERR_OR_NULL(dvdd_lcd_1v8)) {
		pr_err("dvdd_lcd regulator get failed\n");
		err = PTR_ERR(dvdd_lcd_1v8);
		dvdd_lcd_1v8 = NULL;
		goto fail;
	}
#endif
#if PANEL_11_6_AUO_1920_1080
	vdd_ds_1v8 = regulator_get(dev, "vdd_ds_1v8");
	if (IS_ERR_OR_NULL(vdd_ds_1v8)) {
		pr_err("vdd_ds_1v8 regulator get failed\n");
		err = PTR_ERR(vdd_ds_1v8);
		vdd_ds_1v8 = NULL;
		goto fail;
	}
#endif
#if PANEL_10_1_PANASONIC_1920_1200 || \
	PANEL_11_6_AUO_1920_1080 || \
	PANEL_10_1_SHARP_2560_1600
	avdd_lcd_3v3 = regulator_get(dev, "avdd_lcd");
	if (IS_ERR_OR_NULL(avdd_lcd_3v3)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(avdd_lcd_3v3);
		avdd_lcd_3v3 = NULL;
		goto fail;
	}

	vdd_lcd_bl_12v = regulator_get(dev, "vdd_lcd_bl");
	if (IS_ERR_OR_NULL(vdd_lcd_bl_12v)) {
		pr_err("vdd_lcd_bl regulator get failed\n");
		err = PTR_ERR(vdd_lcd_bl_12v);
		vdd_lcd_bl_12v = NULL;
		goto fail;
	}
#endif
	reg_requested = true;
	return 0;
fail:
	return err;
}

static int dalmore_dsi_gpio_get(void)
{
	int err = 0;

	if (gpio_requested)
		return 0;

	err = gpio_request(DSI_PANEL_RST_GPIO, "panel rst");
	if (err < 0) {
		pr_err("panel reset gpio request failed\n");
		goto fail;
	}

	err = gpio_request(DSI_PANEL_BL_EN_GPIO, "panel backlight");
	if (err < 0) {
		pr_err("panel backlight gpio request failed\n");
		goto fail;
	}

	/* free pwm GPIO */
	err = gpio_request(DSI_PANEL_BL_PWM, "panel pwm");
	if (err < 0) {
		pr_err("panel pwm gpio request failed\n");
		goto fail;
	}
	gpio_free(DSI_PANEL_BL_PWM);

#if PANEL_11_6_AUO_1920_1080
	err = gpio_request(en_vdd_bl, "edp bridge 1v2 enable");
	if (err < 0) {
		pr_err("edp bridge 1v2 enable gpio request failed\n");
		goto fail;
	}

	err = gpio_request(lvds_en, "edp bridge 1v8 enable");
	if (err < 0) {
		pr_err("edp bridge 1v8 enable gpio request failed\n");
		goto fail;
	}
#endif
	gpio_requested = true;
	return 0;
fail:
	return err;
}

static int dalmore_dsi_panel_enable(struct device *dev)
{
	int err = 0;

	err = dalmore_dsi_regulator_get(dev);
	if (err < 0) {
		pr_err("dsi regulator get failed\n");
		goto fail;
	}
	err = dalmore_dsi_gpio_get();
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
		goto fail;
	}

	if (vdd_ds_1v8) {
		err = regulator_enable(vdd_ds_1v8);
		if (err < 0) {
			pr_err("vdd_ds_1v8 regulator enable failed\n");
			goto fail;
		}
	}

	if (dvdd_lcd_1v8) {
		err = regulator_enable(dvdd_lcd_1v8);
		if (err < 0) {
			pr_err("dvdd_lcd regulator enable failed\n");
			goto fail;
		}
	}

	if (avdd_lcd_3v3) {
		err = regulator_enable(avdd_lcd_3v3);
		if (err < 0) {
			pr_err("avdd_lcd regulator enable failed\n");
			goto fail;
		}
	}

	if (vdd_lcd_bl_12v) {
		err = regulator_enable(vdd_lcd_bl_12v);
		if (err < 0) {
			pr_err("vdd_lcd_bl regulator enable failed\n");
			goto fail;
		}
	}

	msleep(100);
#if DSI_PANEL_RESET
	gpio_direction_output(DSI_PANEL_RST_GPIO, 1);
	usleep_range(1000, 5000);
	gpio_set_value(DSI_PANEL_RST_GPIO, 0);
	msleep(150);
	gpio_set_value(DSI_PANEL_RST_GPIO, 1);
	msleep(1500);
#endif

	gpio_direction_output(DSI_PANEL_BL_EN_GPIO, 1);

#if PANEL_11_6_AUO_1920_1080
	gpio_direction_output(en_vdd_bl, 1);
	msleep(100);
	gpio_direction_output(lvds_en, 1);
#endif
	return 0;
fail:
	return err;
}

static int dalmore_dsi_panel_disable(void)
{
	gpio_set_value(DSI_PANEL_BL_EN_GPIO, 0);

#if PANEL_11_6_AUO_1920_1080
	gpio_set_value(lvds_en, 0);
	msleep(100);
	gpio_set_value(en_vdd_bl, 0);
#endif
	if (vdd_lcd_bl_12v)
		regulator_disable(vdd_lcd_bl_12v);

	if (avdd_lcd_3v3)
		regulator_disable(avdd_lcd_3v3);

	if (dvdd_lcd_1v8)
		regulator_disable(dvdd_lcd_1v8);

	if (vdd_ds_1v8)
		regulator_disable(vdd_ds_1v8);

	return 0;
}

static int dalmore_dsi_panel_postsuspend(void)
{
	return 0;
}

static struct tegra_dc_mode dalmore_dsi_modes[] = {
#if PANEL_10_1_PANASONIC_1920_1200
	{
		.pclk = 10000000,
		.h_ref_to_sync = 4,
		.v_ref_to_sync = 1,
		.h_sync_width = 16,
		.v_sync_width = 2,
		.h_back_porch = 32,
		.v_back_porch = 16,
		.h_active = 1920,
		.v_active = 1200,
		.h_front_porch = 120,
		.v_front_porch = 17,
	},
#endif
#if PANEL_11_6_AUO_1920_1080
	{
		.pclk = 10000000,
		.h_ref_to_sync = 4,
		.v_ref_to_sync = 1,
		.h_sync_width = 28,
		.v_sync_width = 5,
		.h_back_porch = 148,
		.v_back_porch = 23,
		.h_active = 1920,
		.v_active = 1080,
		.h_front_porch = 66,
		.v_front_porch = 4,
	},
#endif
#if PANEL_10_1_SHARP_2560_1600
	{
		.pclk = 10000000,
		.h_ref_to_sync = 4,
		.v_ref_to_sync = 1,
		.h_sync_width = 16,
		.v_sync_width = 2,
		.h_back_porch = 16,
		.v_back_porch = 33,
		.h_active = 2560,
		.v_active = 1600,
		.h_front_porch = 128,
		.v_front_porch = 10,
	},
#endif
};

static struct tegra_dc_sd_settings sd_settings;

static struct tegra_dc_out dalmore_disp1_out = {
	.type		= TEGRA_DC_OUT_DSI,
	.dsi		= &dalmore_dsi,

	.flags		= DC_CTRL_MODE,
	.sd_settings	= &sd_settings,

	.modes		= dalmore_dsi_modes,
	.n_modes	= ARRAY_SIZE(dalmore_dsi_modes),

	.enable		= dalmore_dsi_panel_enable,
	.disable	= dalmore_dsi_panel_disable,
	.postsuspend	= dalmore_dsi_panel_postsuspend,

#if PANEL_10_1_PANASONIC_1920_1200
	.width		= 217,
	.height		= 135,
#endif
#if PANEL_11_6_AUO_1920_1080
	.width		= 256,
	.height		= 144,
#endif
#if PANEL_10_1_SHARP_2560_1600
	.width		= 216,
	.height		= 135,
#endif
};

static int dalmore_hdmi_enable(struct device *dev)
{
	int ret;
	if (!dalmore_hdmi_reg) {
			dalmore_hdmi_reg = regulator_get(dev, "avdd_hdmi");
			if (IS_ERR_OR_NULL(dalmore_hdmi_reg)) {
				pr_err("hdmi: couldn't get regulator avdd_hdmi\n");
				dalmore_hdmi_reg = NULL;
				return PTR_ERR(dalmore_hdmi_reg);
			}
	}
	ret = regulator_enable(dalmore_hdmi_reg);
	if (ret < 0) {
		pr_err("hdmi: couldn't enable regulator avdd_hdmi\n");
		return ret;
	}
	if (!dalmore_hdmi_pll) {
		dalmore_hdmi_pll = regulator_get(dev, "avdd_hdmi_pll");
		if (IS_ERR_OR_NULL(dalmore_hdmi_pll)) {
			pr_err("hdmi: couldn't get regulator avdd_hdmi_pll\n");
			dalmore_hdmi_pll = NULL;
			regulator_put(dalmore_hdmi_reg);
			dalmore_hdmi_reg = NULL;
			return PTR_ERR(dalmore_hdmi_pll);
		}
	}
	ret = regulator_enable(dalmore_hdmi_pll);
	if (ret < 0) {
		pr_err("hdmi: couldn't enable regulator avdd_hdmi_pll\n");
		return ret;
	}
	return 0;
}

static int dalmore_hdmi_disable(void)
{
	if (dalmore_hdmi_reg) {
		regulator_disable(dalmore_hdmi_reg);
		regulator_put(dalmore_hdmi_reg);
		dalmore_hdmi_reg = NULL;
	}

	if (dalmore_hdmi_pll) {
		regulator_disable(dalmore_hdmi_pll);
		regulator_put(dalmore_hdmi_pll);
		dalmore_hdmi_pll = NULL;
	}

	return 0;
}

static int dalmore_hdmi_postsuspend(void)
{
	if (dalmore_hdmi_vddio) {
		regulator_disable(dalmore_hdmi_vddio);
		regulator_put(dalmore_hdmi_vddio);
		dalmore_hdmi_vddio = NULL;
	}
	return 0;
}

static int dalmore_hdmi_hotplug_init(struct device *dev)
{
	if (!dalmore_hdmi_vddio) {
		dalmore_hdmi_vddio = regulator_get(dev, "vdd_hdmi_5v0");
		if (WARN_ON(IS_ERR(dalmore_hdmi_vddio))) {
			pr_err("%s: couldn't get regulator vdd_hdmi_5v0: %ld\n",
				__func__, PTR_ERR(dalmore_hdmi_vddio));
				dalmore_hdmi_vddio = NULL;
		} else {
			regulator_enable(dalmore_hdmi_vddio);
		}
	}

	return 0;
}

static struct tegra_dc_out dalmore_disp2_out = {
	.type		= TEGRA_DC_OUT_HDMI,
	.flags		= TEGRA_DC_OUT_HOTPLUG_HIGH,
	.parent_clk	= "pll_d2_out0",

	.dcc_bus	= 3,
	.hotplug_gpio	= dalmore_hdmi_hpd,

	.max_pixclock	= KHZ2PICOS(148500),

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,

	.enable		= dalmore_hdmi_enable,
	.disable	= dalmore_hdmi_disable,
	.postsuspend	= dalmore_hdmi_postsuspend,
	.hotplug_init	= dalmore_hdmi_hotplug_init,
};

static struct tegra_fb_data dalmore_disp1_fb_data = {
	.win		= 0,
	.bits_per_pixel = 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
#if PANEL_10_1_PANASONIC_1920_1200
	.xres		= 1920,
	.yres		= 1200,
#endif
#if PANEL_11_6_AUO_1920_1080
	.xres		= 1920,
	.yres		= 1080,
#endif
#if PANEL_10_1_SHARP_2560_1600
	.xres		= 2560,
	.yres		= 1600,
#endif
};

#ifdef CONFIG_TEGRA_DC_CMU
#if PANEL_10_1_PANASONIC_1920_1200
static struct tegra_dc_cmu dalmore_panasonic_cmu = {
	/* lut1 maps sRGB to linear space. */
	{
	0,    1,    2,    4,    5,    6,    7,    9,
	10,   11,   12,   14,   15,   16,   18,   20,
	21,   23,   25,   27,   29,   31,   33,   35,
	37,   40,   42,   45,   48,   50,   53,   56,
	59,   62,   66,   69,   72,   76,   79,   83,
	87,   91,   95,   99,   103,  107,  112,  116,
	121,  126,  131,  136,  141,  146,  151,  156,
	162,  168,  173,  179,  185,  191,  197,  204,
	210,  216,  223,  230,  237,  244,  251,  258,
	265,  273,  280,  288,  296,  304,  312,  320,
	329,  337,  346,  354,  363,  372,  381,  390,
	400,  409,  419,  428,  438,  448,  458,  469,
	479,  490,  500,  511,  522,  533,  544,  555,
	567,  578,  590,  602,  614,  626,  639,  651,
	664,  676,  689,  702,  715,  728,  742,  755,
	769,  783,  797,  811,  825,  840,  854,  869,
	884,  899,  914,  929,  945,  960,  976,  992,
	1008, 1024, 1041, 1057, 1074, 1091, 1108, 1125,
	1142, 1159, 1177, 1195, 1213, 1231, 1249, 1267,
	1286, 1304, 1323, 1342, 1361, 1381, 1400, 1420,
	1440, 1459, 1480, 1500, 1520, 1541, 1562, 1582,
	1603, 1625, 1646, 1668, 1689, 1711, 1733, 1755,
	1778, 1800, 1823, 1846, 1869, 1892, 1916, 1939,
	1963, 1987, 2011, 2035, 2059, 2084, 2109, 2133,
	2159, 2184, 2209, 2235, 2260, 2286, 2312, 2339,
	2365, 2392, 2419, 2446, 2473, 2500, 2527, 2555,
	2583, 2611, 2639, 2668, 2696, 2725, 2754, 2783,
	2812, 2841, 2871, 2901, 2931, 2961, 2991, 3022,
	3052, 3083, 3114, 3146, 3177, 3209, 3240, 3272,
	3304, 3337, 3369, 3402, 3435, 3468, 3501, 3535,
	3568, 3602, 3636, 3670, 3705, 3739, 3774, 3809,
	3844, 3879, 3915, 3950, 3986, 4022, 4059, 4095,
	},
	/* csc */
	{
		0x12D, 0x3BE, 0x014, /* 1.176 -0.254 0.078 */
		0x3F6, 0x116, 0x3E6, /* -0.037 1.086 -0.098 */
		0x3FE, 0x3F8, 0x0DB, /* -0.003 -0.027 0.859 */
	},
	/* lut2 maps linear space to sRGB */
	{
		0,    1,    2,    2,    3,    4,    5,    6,
		6,    7,    8,    9,    10,   10,   11,   12,
		13,   13,   14,   15,   15,   16,   16,   17,
		18,   18,   19,   19,   20,   20,   21,   21,
		22,   22,   23,   23,   23,   24,   24,   25,
		25,   25,   26,   26,   27,   27,   27,   28,
		28,   29,   29,   29,   30,   30,   30,   31,
		31,   31,   32,   32,   32,   33,   33,   33,
		34,   34,   34,   34,   35,   35,   35,   36,
		36,   36,   37,   37,   37,   37,   38,   38,
		38,   38,   39,   39,   39,   40,   40,   40,
		40,   41,   41,   41,   41,   42,   42,   42,
		42,   43,   43,   43,   43,   43,   44,   44,
		44,   44,   45,   45,   45,   45,   46,   46,
		46,   46,   46,   47,   47,   47,   47,   48,
		48,   48,   48,   48,   49,   49,   49,   49,
		49,   50,   50,   50,   50,   50,   51,   51,
		51,   51,   51,   52,   52,   52,   52,   52,
		53,   53,   53,   53,   53,   54,   54,   54,
		54,   54,   55,   55,   55,   55,   55,   55,
		56,   56,   56,   56,   56,   57,   57,   57,
		57,   57,   57,   58,   58,   58,   58,   58,
		58,   59,   59,   59,   59,   59,   59,   60,
		60,   60,   60,   60,   60,   61,   61,   61,
		61,   61,   61,   62,   62,   62,   62,   62,
		62,   63,   63,   63,   63,   63,   63,   64,
		64,   64,   64,   64,   64,   64,   65,   65,
		65,   65,   65,   65,   66,   66,   66,   66,
		66,   66,   66,   67,   67,   67,   67,   67,
		67,   67,   68,   68,   68,   68,   68,   68,
		68,   69,   69,   69,   69,   69,   69,   69,
		70,   70,   70,   70,   70,   70,   70,   71,
		71,   71,   71,   71,   71,   71,   72,   72,
		72,   72,   72,   72,   72,   72,   73,   73,
		73,   73,   73,   73,   73,   74,   74,   74,
		74,   74,   74,   74,   74,   75,   75,   75,
		75,   75,   75,   75,   75,   76,   76,   76,
		76,   76,   76,   76,   77,   77,   77,   77,
		77,   77,   77,   77,   78,   78,   78,   78,
		78,   78,   78,   78,   78,   79,   79,   79,
		79,   79,   79,   79,   79,   80,   80,   80,
		80,   80,   80,   80,   80,   81,   81,   81,
		81,   81,   81,   81,   81,   81,   82,   82,
		82,   82,   82,   82,   82,   82,   83,   83,
		83,   83,   83,   83,   83,   83,   83,   84,
		84,   84,   84,   84,   84,   84,   84,   84,
		85,   85,   85,   85,   85,   85,   85,   85,
		85,   86,   86,   86,   86,   86,   86,   86,
		86,   86,   87,   87,   87,   87,   87,   87,
		87,   87,   87,   88,   88,   88,   88,   88,
		88,   88,   88,   88,   88,   89,   89,   89,
		89,   89,   89,   89,   89,   89,   90,   90,
		90,   90,   90,   90,   90,   90,   90,   90,
		91,   91,   91,   91,   91,   91,   91,   91,
		91,   91,   92,   92,   92,   92,   92,   92,
		92,   92,   92,   92,   93,   93,   93,   93,
		93,   93,   93,   93,   93,   93,   94,   94,
		94,   94,   94,   94,   94,   94,   94,   94,
		95,   95,   95,   95,   95,   95,   95,   95,
		95,   95,   96,   96,   96,   96,   96,   96,
		96,   96,   96,   96,   96,   97,   97,   97,
		97,   97,   97,   97,   97,   97,   97,   98,
		98,   98,   98,   98,   98,   98,   98,   98,
		98,   98,   99,   99,   99,   99,   99,   99,
		99,   100,  101,  101,  102,  103,  103,  104,
		105,  105,  106,  107,  107,  108,  109,  109,
		110,  111,  111,  112,  113,  113,  114,  115,
		115,  116,  116,  117,  118,  118,  119,  119,
		120,  120,  121,  122,  122,  123,  123,  124,
		124,  125,  126,  126,  127,  127,  128,  128,
		129,  129,  130,  130,  131,  131,  132,  132,
		133,  133,  134,  134,  135,  135,  136,  136,
		137,  137,  138,  138,  139,  139,  140,  140,
		141,  141,  142,  142,  143,  143,  144,  144,
		145,  145,  145,  146,  146,  147,  147,  148,
		148,  149,  149,  150,  150,  150,  151,  151,
		152,  152,  153,  153,  153,  154,  154,  155,
		155,  156,  156,  156,  157,  157,  158,  158,
		158,  159,  159,  160,  160,  160,  161,  161,
		162,  162,  162,  163,  163,  164,  164,  164,
		165,  165,  166,  166,  166,  167,  167,  167,
		168,  168,  169,  169,  169,  170,  170,  170,
		171,  171,  172,  172,  172,  173,  173,  173,
		174,  174,  174,  175,  175,  176,  176,  176,
		177,  177,  177,  178,  178,  178,  179,  179,
		179,  180,  180,  180,  181,  181,  182,  182,
		182,  183,  183,  183,  184,  184,  184,  185,
		185,  185,  186,  186,  186,  187,  187,  187,
		188,  188,  188,  189,  189,  189,  189,  190,
		190,  190,  191,  191,  191,  192,  192,  192,
		193,  193,  193,  194,  194,  194,  195,  195,
		195,  196,  196,  196,  196,  197,  197,  197,
		198,  198,  198,  199,  199,  199,  200,  200,
		200,  200,  201,  201,  201,  202,  202,  202,
		202,  203,  203,  203,  204,  204,  204,  205,
		205,  205,  205,  206,  206,  206,  207,  207,
		207,  207,  208,  208,  208,  209,  209,  209,
		209,  210,  210,  210,  211,  211,  211,  211,
		212,  212,  212,  213,  213,  213,  213,  214,
		214,  214,  214,  215,  215,  215,  216,  216,
		216,  216,  217,  217,  217,  217,  218,  218,
		218,  219,  219,  219,  219,  220,  220,  220,
		220,  221,  221,  221,  221,  222,  222,  222,
		223,  223,  223,  223,  224,  224,  224,  224,
		225,  225,  225,  225,  226,  226,  226,  226,
		227,  227,  227,  227,  228,  228,  228,  228,
		229,  229,  229,  229,  230,  230,  230,  230,
		231,  231,  231,  231,  232,  232,  232,  232,
		233,  233,  233,  233,  234,  234,  234,  234,
		235,  235,  235,  235,  236,  236,  236,  236,
		237,  237,  237,  237,  238,  238,  238,  238,
		239,  239,  239,  239,  240,  240,  240,  240,
		240,  241,  241,  241,  241,  242,  242,  242,
		242,  243,  243,  243,  243,  244,  244,  244,
		244,  244,  245,  245,  245,  245,  246,  246,
		246,  246,  247,  247,  247,  247,  247,  248,
		248,  248,  248,  249,  249,  249,  249,  249,
		250,  250,  250,  250,  251,  251,  251,  251,
		251,  252,  252,  252,  252,  253,  253,  253,
		253,  253,  254,  254,  254,  254,  255,  255,
	},
};
#endif
#endif

static struct tegra_dc_platform_data dalmore_disp1_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &dalmore_disp1_out,
	.fb		= &dalmore_disp1_fb_data,
	.emc_clk_rate	= 204000000,
#ifdef CONFIG_TEGRA_DC_CMU
	.cmu_enable	= 1,
#if PANEL_10_1_PANASONIC_1920_1200
	.cmu = &dalmore_panasonic_cmu,
#endif
#endif
};

static struct tegra_fb_data dalmore_disp2_fb_data = {
	.win		= 0,
	.xres		= 1024,
	.yres		= 600,
	.bits_per_pixel = 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
};

static struct tegra_dc_platform_data dalmore_disp2_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &dalmore_disp2_out,
	.fb		= &dalmore_disp2_fb_data,
	.emc_clk_rate	= 300000000,
};

static struct platform_device dalmore_disp2_device = {
	.name		= "tegradc",
	.id		= 1,
	.resource	= dalmore_disp2_resources,
	.num_resources	= ARRAY_SIZE(dalmore_disp2_resources),
	.dev = {
		.platform_data = &dalmore_disp2_pdata,
	},
};

static struct platform_device dalmore_disp1_device = {
	.name		= "tegradc",
	.id		= 0,
	.resource	= dalmore_disp1_resources,
	.num_resources	= ARRAY_SIZE(dalmore_disp1_resources),
	.dev = {
		.platform_data = &dalmore_disp1_pdata,
	},
};

static struct nvmap_platform_carveout dalmore_carveouts[] = {
	[0] = {
		.name		= "iram",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_IRAM,
		.base		= TEGRA_IRAM_BASE + TEGRA_RESET_HANDLER_SIZE,
		.size		= TEGRA_IRAM_SIZE - TEGRA_RESET_HANDLER_SIZE,
		.buddy_size	= 0, /* no buddy allocation for IRAM */
	},
	[1] = {
		.name		= "generic-0",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_GENERIC,
		.base		= 0, /* Filled in by dalmore_panel_init() */
		.size		= 0, /* Filled in by dalmore_panel_init() */
		.buddy_size	= SZ_32K,
	},
	[2] = {
		.name		= "vpr",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_VPR,
		.base		= 0, /* Filled in by dalmore_panel_init() */
		.size		= 0, /* Filled in by dalmore_panel_init() */
		.buddy_size	= SZ_32K,
	},
};

static struct nvmap_platform_data dalmore_nvmap_data = {
	.carveouts	= dalmore_carveouts,
	.nr_carveouts	= ARRAY_SIZE(dalmore_carveouts),
};

static struct platform_device dalmore_nvmap_device __initdata = {
	.name	= "tegra-nvmap",
	.id	= -1,
	.dev	= {
		.platform_data = &dalmore_nvmap_data,
	},
};

static int dalmore_disp1_bl_notify(struct device *unused, int brightness)
{
	int cur_sd_brightness = atomic_read(&sd_brightness);

	/* Apply any backlight response curve */
	if (brightness > 255)
		pr_info("Error: Brightness > 255!\n");
	else
		brightness = bl_output[brightness];

	/* SD brightness is a percentage */
	brightness = (brightness * cur_sd_brightness) / 255;

	return brightness;
}

static int dalmore_disp1_check_fb(struct device *dev, struct fb_info *info)
{
	return info->device == &dalmore_disp1_device.dev;
}

static struct platform_pwm_backlight_data dalmore_disp1_bl_data = {
	.pwm_id		= 1,
	.max_brightness	= 255,
	.dft_brightness	= 224,
	.pwm_period_ns	= 1000000,
	.notify		= dalmore_disp1_bl_notify,
	/* Only toggle backlight on fb blank notifications for disp1 */
	.check_fb	= dalmore_disp1_check_fb,
};

static struct platform_device __maybe_unused
		dalmore_disp1_bl_device __initdata = {
	.name	= "pwm-backlight",
	.id	= -1,
	.dev	= {
		.platform_data = &dalmore_disp1_bl_data,
	},
};

static struct tegra_dc_sd_settings dalmore_sd_settings = {
	.enable = 1, /* enabled by default. */
	.use_auto_pwm = false,
	.hw_update_delay = 0,
	.bin_width = -1,
	.aggressiveness = 5,
	.use_vid_luma = false,
	.k_limit_enable = true,
	.k_limit = 200,
	.sd_window_enable = false,
	.soft_clipping_enable = true,
	/* Low soft clipping threshold to compensate for aggressive k_limit */
	.soft_clipping_threshold = 128,
	.smooth_k_enable = true,
	.smooth_k_incr = 64,
	/* Default video coefficients */
	.coeff = {5, 9, 2},
	.fc = {0, 0},
	/* Immediate backlight changes */
	.blp = {1024, 255},
	/* Gammas: R: 2.2 G: 2.2 B: 2.2 */
	/* Default BL TF */
	.bltf = {
			{
				{57, 65, 73, 82},
				{92, 103, 114, 125},
				{138, 150, 164, 178},
				{193, 208, 224, 241},
			},
		},
	/* Default LUT */
	.lut = {
			{
				{255, 255, 255},
				{199, 199, 199},
				{153, 153, 153},
				{116, 116, 116},
				{85, 85, 85},
				{59, 59, 59},
				{36, 36, 36},
				{17, 17, 17},
				{0, 0, 0},
			},
		},
	.sd_brightness = &sd_brightness,
	.bl_device_name = "pwm-backlight",
	.use_vpulse2 = true,
};

#if PANEL_11_6_AUO_1920_1080
static struct i2c_board_info dalmore_tc358770_dsi2edp_board_info __initdata = {
		I2C_BOARD_INFO("tc358770_dsi2edp", 0x68),
};
#endif

static struct platform_device __maybe_unused
			*dalmore_bl_device[] __initdata = {
	&tegra_pwfm1_device,
	&dalmore_disp1_bl_device,
};

int __init dalmore_panel_init(void)
{
	int err = 0;
	struct resource __maybe_unused *res;
	struct platform_device *phost1x;

	sd_settings = dalmore_sd_settings;
#ifdef CONFIG_TEGRA_NVMAP
	dalmore_carveouts[1].base = tegra_carveout_start;
	dalmore_carveouts[1].size = tegra_carveout_size;
	dalmore_carveouts[2].base = tegra_vpr_start;
	dalmore_carveouts[2].size = tegra_vpr_size;

	err = platform_device_register(&dalmore_nvmap_device);
	if (err) {
		pr_err("nvmap device registration failed\n");
		return err;
	}
#endif

	phost1x = dalmore_host1x_init();
	if (!phost1x) {
		pr_err("host1x devices registration failed\n");
		return -EINVAL;
	}

	gpio_request(dalmore_hdmi_hpd, "hdmi_hpd");
	gpio_direction_input(dalmore_hdmi_hpd);

	res = platform_get_resource_byname(&dalmore_disp1_device,
					 IORESOURCE_MEM, "fbmem");
	res->start = tegra_fb_start;
	res->end = tegra_fb_start + tegra_fb_size - 1;

	/* Copy the bootloader fb to the fb. */
	tegra_move_framebuffer(tegra_fb_start, tegra_bootloader_fb_start,
			min(tegra_fb_size, tegra_bootloader_fb_size));

	res = platform_get_resource_byname(&dalmore_disp2_device,
		IORESOURCE_MEM, "fbmem");
	res->start = tegra_fb2_start;
	res->end = tegra_fb2_start + tegra_fb2_size - 1;

	dalmore_disp1_device.dev.parent = &phost1x->dev;
	err = platform_device_register(&dalmore_disp1_device);
	if (err) {
		pr_err("disp1 device registration failed\n");
		return err;
	}

	dalmore_disp2_device.dev.parent = &phost1x->dev;
	err = platform_device_register(&dalmore_disp2_device);
	if (err) {
		pr_err("disp2 device registration failed\n");
		return err;
	}

#if IS_EXTERNAL_PWM
	err = platform_add_devices(dalmore_bl_device,
				ARRAY_SIZE(dalmore_bl_device));
	if (err) {
		pr_err("disp1 bl device registration failed");
		return err;
	}
#endif
#if PANEL_11_6_AUO_1920_1080
	i2c_register_board_info(0, &dalmore_tc358770_dsi2edp_board_info, 1);
#endif

#ifdef CONFIG_TEGRA_NVAVP
	nvavp_device.dev.parent = &phost1x->dev;
	err = platform_device_register(&nvavp_device);
	if (err) {
		pr_err("nvavp device registration failed\n");
		return err;
	}
#endif
	return err;
}
#else
int __init dalmore_panel_init(void)
{
	if (dalmore_host1x_init())
		return 0;
	else
		return -EINVAL;
}
#endif