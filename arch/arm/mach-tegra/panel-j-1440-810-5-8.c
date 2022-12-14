/*
 * arch/arm/mach-tegra/panel-j-1440-810-5-8.c
 *
 * Copyright (c) 2013, NVIDIA Corporation. All rights reserved.
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
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/tegra_pwm_bl.h>
#include <linux/regulator/consumer.h>
#include <linux/pwm_backlight.h>

#include <mach/dc.h>
#include <asm/mach-types.h>

#include "devices.h"
#include "board-panel.h"

#include "gpio-names.h"
#define DSI_PANEL_EN_GPIO	TEGRA_GPIO_PQ2

#define DSI_PANEL_RESET		1

#define DC_CTRL_MODE	TEGRA_DC_OUT_CONTINUOUS_MODE

static struct tegra_dsi_out dsi_j_1440_810_5_8_pdata;

static bool reg_requested;
static bool gpio_requested;
static struct platform_device *disp_device;

static struct regulator *vdd_lcd_s_1v8;
static struct regulator *vdd_lcd_bl;
static struct regulator *vdd_lcd_bl_en;
static struct regulator *avdd_lcd_3v0_2v8;

#ifdef CONFIG_TEGRA_DC_CMU
static struct tegra_dc_cmu dsi_j_1440_810_5_8_cmu = {
	/* lut1 maps sRGB to linear space. */
	{
		0,    1,    2,	  4,	5,    6,    7,	  9,
		10,   11,   12,   14,	15,   16,   18,   20,
		21,   23,   25,   27,	29,   31,   33,   35,
		37,   40,   42,   45,	48,   50,   53,   56,
		59,   62,   66,   69,	72,   76,   79,   83,
		87,   91,   95,   99,	103,  107,  112,  116,
		121,  126,  131,  136,	141,  146,  151,  156,
		162,  168,  173,  179,	185,  191,  197,  204,
		210,  216,  223,  230,	237,  244,  251,  258,
		265,  273,  280,  288,	296,  304,  312,  320,
		329,  337,  346,  354,	363,  372,  381,  390,
		400,  409,  419,  428,	438,  448,  458,  469,
		479,  490,  500,  511,	522,  533,  544,  555,
		567,  578,  590,  602,	614,  626,  639,  651,
		664,  676,  689,  702,	715,  728,  742,  755,
		769,  783,  797,  811,	825,  840,  854,  869,
		884,  899,  914,  929,	945,  960,  976,  992,
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
		0x100, 0x0,   0x0,
		0x0,   0x100, 0x0,
		0x0,   0x0,   0x100,
	},
	/* lut2 maps linear space to sRGB*/
	{
		0, 0, 1, 2, 3, 3, 4, 5,
		6, 6, 7, 8, 8, 9, 10, 10,
		11, 12, 12, 13, 13, 14, 14, 15,
		16, 16, 17, 17, 18, 18, 19, 19,
		19, 20, 20, 21, 21, 22, 22, 22,
		23, 23, 24, 24, 24, 25, 25, 25,
		26, 26, 27, 27, 27, 28, 28, 28,
		28, 29, 29, 29, 30, 30, 30, 31,
		31, 31, 31, 32, 32, 32, 33, 33,
		33, 33, 34, 34, 34, 35, 35, 35,
		35, 36, 36, 36, 36, 37, 37, 37,
		38, 38, 38, 38, 39, 39, 39, 39,
		40, 40, 40, 40, 40, 41, 41, 41,
		41, 42, 42, 42, 42, 43, 43, 43,
		43, 43, 44, 44, 44, 44, 45, 45,
		45, 45, 45, 46, 46, 46, 46, 46,
		47, 47, 47, 47, 47, 48, 48, 48,
		48, 48, 49, 49, 49, 49, 49, 49,
		50, 50, 50, 50, 50, 50, 51, 51,
		51, 51, 51, 51, 52, 52, 52, 52,
		52, 52, 53, 53, 53, 53, 53, 53,
		54, 54, 54, 54, 54, 54, 54, 55,
		55, 55, 55, 55, 55, 55, 55, 56,
		56, 56, 56, 56, 56, 56, 57, 57,
		57, 57, 57, 57, 57, 57, 58, 58,
		58, 58, 58, 58, 58, 58, 58, 59,
		59, 59, 59, 59, 59, 59, 59, 59,
		60, 60, 60, 60, 60, 60, 60, 60,
		60, 61, 61, 61, 61, 61, 61, 61,
		61, 61, 61, 62, 62, 62, 62, 62,
		62, 62, 62, 62, 62, 63, 63, 63,
		63, 63, 63, 63, 63, 63, 63, 63,
		64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 65, 65, 65, 65, 65,
		65, 65, 65, 65, 65, 65, 66, 66,
		66, 66, 66, 66, 66, 66, 66, 66,
		66, 66, 67, 67, 67, 67, 67, 67,
		67, 67, 67, 67, 67, 67, 68, 68,
		68, 68, 68, 68, 68, 68, 68, 68,
		68, 68, 69, 69, 69, 69, 69, 69,
		69, 69, 69, 69, 69, 69, 70, 70,
		70, 70, 70, 70, 70, 70, 70, 70,
		70, 70, 70, 71, 71, 71, 71, 71,
		71, 71, 71, 71, 71, 71, 71, 71,
		72, 72, 72, 72, 72, 72, 72, 72,
		72, 72, 72, 72, 72, 73, 73, 73,
		73, 73, 73, 73, 73, 73, 73, 73,
		73, 73, 73, 74, 74, 74, 74, 74,
		74, 74, 74, 74, 74, 74, 74, 74,
		75, 75, 75, 75, 75, 75, 75, 75,
		75, 75, 75, 75, 75, 75, 76, 76,
		76, 76, 76, 76, 76, 76, 76, 76,
		76, 76, 76, 76, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 78, 78, 78, 78, 78, 78,
		78, 78, 78, 78, 78, 78, 78, 78,
		79, 79, 79, 79, 79, 79, 79, 79,
		79, 79, 79, 79, 79, 79, 80, 80,
		80, 80, 80, 80, 80, 80, 80, 80,
		80, 80, 80, 80, 81, 81, 81, 81,
		81, 81, 81, 81, 81, 81, 81, 81,
		81, 81, 82, 82, 82, 82, 82, 82,
		82, 82, 82, 82, 82, 82, 82, 82,
		83, 83, 83, 83, 83, 83, 83, 83,
		84, 84, 85, 85, 86, 86, 87, 88,
		88, 89, 89, 90, 90, 91, 92, 92,
		93, 93, 94, 94, 95, 95, 96, 96,
		97, 97, 98, 98, 99, 99, 100, 100,
		101, 101, 102, 102, 103, 103, 104, 104,
		105, 105, 106, 106, 107, 107, 107, 108,
		108, 109, 109, 110, 110, 111, 111, 111,
		112, 112, 113, 113, 114, 114, 114, 115,
		115, 116, 116, 117, 117, 117, 118, 118,
		119, 119, 119, 120, 120, 121, 121, 121,
		122, 122, 123, 123, 123, 124, 124, 125,
		125, 126, 126, 126, 127, 127, 128, 128,
		128, 129, 129, 129, 130, 130, 131, 131,
		131, 132, 132, 133, 133, 133, 134, 134,
		135, 135, 135, 136, 136, 137, 137, 137,
		138, 138, 138, 139, 139, 140, 140, 140,
		141, 141, 142, 142, 142, 143, 143, 143,
		144, 144, 145, 145, 145, 146, 146, 146,
		147, 147, 147, 148, 148, 149, 149, 149,
		150, 150, 150, 151, 151, 151, 152, 152,
		153, 153, 153, 154, 154, 154, 155, 155,
		156, 156, 156, 157, 157, 157, 158, 158,
		159, 159, 159, 160, 160, 160, 161, 161,
		162, 162, 162, 163, 163, 164, 164, 164,
		165, 165, 166, 166, 166, 167, 167, 168,
		168, 168, 169, 169, 170, 170, 170, 171,
		171, 172, 172, 172, 173, 173, 173, 174,
		174, 175, 175, 175, 176, 176, 176, 177,
		177, 177, 178, 178, 178, 179, 179, 179,
		180, 180, 180, 180, 181, 181, 181, 182,
		182, 182, 182, 183, 183, 183, 184, 184,
		184, 184, 185, 185, 185, 185, 186, 186,
		186, 186, 187, 187, 187, 187, 188, 188,
		188, 188, 189, 189, 189, 190, 190, 190,
		190, 191, 191, 191, 191, 192, 192, 192,
		193, 193, 193, 193, 194, 194, 194, 195,
		195, 195, 195, 196, 196, 196, 197, 197,
		197, 198, 198, 198, 198, 199, 199, 199,
		200, 200, 200, 201, 201, 201, 202, 202,
		202, 203, 203, 204, 204, 204, 205, 205,
		205, 206, 206, 206, 207, 207, 208, 208,
		208, 209, 209, 209, 210, 210, 211, 211,
		211, 212, 212, 213, 213, 213, 214, 214,
		215, 215, 215, 216, 216, 217, 217, 217,
		218, 218, 218, 219, 219, 220, 220, 220,
		221, 221, 221, 222, 222, 222, 223, 223,
		223, 224, 224, 224, 225, 225, 225, 225,
		226, 226, 226, 226, 227, 227, 227, 227,
		228, 228, 228, 228, 229, 229, 229, 229,
		229, 230, 230, 230, 230, 231, 231, 231,
		231, 232, 232, 232, 233, 233, 233, 233,
		234, 234, 234, 235, 235, 235, 236, 236,
		236, 237, 237, 238, 238, 239, 239, 239,
		240, 240, 241, 241, 242, 242, 243, 244,
		244, 245, 245, 246, 247, 247, 248, 249,
		250, 250, 251, 252, 253, 254, 254, 255,
	},
};
#endif

static int dsi_j_1440_810_5_8_bl_notify(struct device *unused, int brightness)
{
	int cur_sd_brightness = atomic_read(&sd_brightness);

	/* SD brightness is a percentage */
	brightness = (brightness * cur_sd_brightness) / 255;

	/* Apply any backlight response curve */
	if (brightness > 255)
		pr_info("Error: Brightness > 255!\n");

	return brightness;
}

static int dsi_j_1440_810_5_8_check_fb(struct device *dev, struct fb_info *info)
{
	return info->device == &disp_device->dev;
}

static struct platform_pwm_backlight_data dsi_j_1440_810_5_8_bl_data = {
	.pwm_id		= 1,
	.max_brightness	= 255,
	.dft_brightness	= 77,
	.pwm_period_ns	= 40000,
	.notify		= dsi_j_1440_810_5_8_bl_notify,
	/* Only toggle backlight on fb blank notifications for disp1 */
	.check_fb	= dsi_j_1440_810_5_8_check_fb,
};

static struct platform_device __maybe_unused dsi_j_1440_810_5_8_bl_device = {
	.name	= "pwm-backlight",
	.id	= -1,
	.dev	= {
		.platform_data = &dsi_j_1440_810_5_8_bl_data,
	},
};

static struct platform_device __maybe_unused *loki_bl_device[] __initdata = {
	&tegra_pwfm_device,
	&dsi_j_1440_810_5_8_bl_device,
};

static struct tegra_dc_mode dsi_j_1440_810_5_8_modes[] = {
	{
		.pclk = 82833000,
		.h_ref_to_sync = 1,
		.v_ref_to_sync = 1,
		.h_sync_width = 48,
		.v_sync_width = 4,
		.h_back_porch = 48,
		.v_back_porch = 8,
		.h_active = 1440,
		.v_active = 810,
		.h_front_porch = 128,
		.v_front_porch = 8,
	},
};

static int dsi_j_1440_810_5_8_reg_get(struct device *dev)
{
	int err = 0;

	if (reg_requested)
		return 0;

	avdd_lcd_3v0_2v8 = regulator_get(dev, "avdd_lcd");
	if (IS_ERR_OR_NULL(avdd_lcd_3v0_2v8)) {
		pr_err("avdd_lcd regulator get failed\n");
		err = PTR_ERR(avdd_lcd_3v0_2v8);
		avdd_lcd_3v0_2v8 = NULL;
		goto fail;
	}
	vdd_lcd_s_1v8 = regulator_get(dev, "dvdd_lcd");
	if (IS_ERR_OR_NULL(vdd_lcd_s_1v8)) {
		pr_err("vdd_lcd_1v8_s regulator get failed\n");
		err = PTR_ERR(vdd_lcd_s_1v8);
		vdd_lcd_s_1v8 = NULL;
		goto fail;
	}

	if (machine_is_dalmore()) {
		vdd_lcd_bl = regulator_get(dev, "vdd_lcd_bl");
		if (IS_ERR_OR_NULL(vdd_lcd_bl)) {
			pr_err("vdd_lcd_bl regulator get failed\n");
			err = PTR_ERR(vdd_lcd_bl);
			vdd_lcd_bl = NULL;
			goto fail;
		}
	}

	vdd_lcd_bl_en = regulator_get(dev, "vdd_lcd_bl_en");
	if (IS_ERR_OR_NULL(vdd_lcd_bl_en)) {
		pr_err("vdd_lcd_bl_en regulator get failed\n");
		err = PTR_ERR(vdd_lcd_bl_en);
		vdd_lcd_bl_en = NULL;
		goto fail;
	}
	reg_requested = true;
	return 0;
fail:
	return err;
}

static int dsi_j_1440_810_5_8_gpio_get(void)
{
	int err = 0;

	if (gpio_requested)
		return 0;

	err = gpio_request(dsi_j_1440_810_5_8_pdata.dsi_panel_rst_gpio,
		"panel rst");
	if (err < 0) {
		pr_err("panel reset gpio request failed\n");
		goto fail;
	}

	err = gpio_request(DSI_PANEL_EN_GPIO, "panel en");
	if (err < 0) {
		pr_err("panel en gpio request failed\n");
		goto fail;
	}

	gpio_requested = true;
	return 0;
fail:
	return err;
}

static struct tegra_dsi_cmd dsi_j_1440_810_5_8_init_cmd[] = {
	/* panel exit_sleep_mode sequence */
	DSI_CMD_SHORT(DSI_DCS_WRITE_0_PARAM, DSI_DCS_EXIT_SLEEP_MODE, 0x0),
	DSI_SEND_FRAME(5),
	DSI_DLY_MS(20),

	/* panel set_display_on sequence */
	DSI_CMD_SHORT(DSI_DCS_WRITE_0_PARAM, DSI_DCS_SET_DISPLAY_ON, 0x0),
	DSI_DLY_MS(20),
};

static struct tegra_dsi_cmd dsi_j_1440_810_5_8_suspend_cmd[] = {
	/* panel set_display_off sequence */
	DSI_CMD_SHORT(DSI_DCS_WRITE_0_PARAM, DSI_DCS_SET_DISPLAY_OFF, 0x0),

	/* panel enter_sleep_mode sequence*/
	DSI_CMD_SHORT(DSI_DCS_WRITE_0_PARAM, DSI_DCS_ENTER_SLEEP_MODE, 0x0),
	DSI_DLY_MS(60),
};

static int dsi_j_1440_810_5_8_enable(struct device *dev)
{
	return 0;
}

static int dsi_j_1440_810_5_8_postpoweron(struct device *dev)
{
	int err = 0;

	err = dsi_j_1440_810_5_8_reg_get(dev);
	if (err < 0) {
		pr_err("dsi regulator get failed\n");
		goto fail;
	}
	err = dsi_j_1440_810_5_8_gpio_get();
	if (err < 0) {
		pr_err("dsi gpio request failed\n");
		goto fail;
	}
	gpio_direction_output(dsi_j_1440_810_5_8_pdata.dsi_panel_rst_gpio, 0);
	gpio_direction_output(DSI_PANEL_EN_GPIO, 0);

	if (vdd_lcd_s_1v8) {
		err = regulator_enable(vdd_lcd_s_1v8);
		if (err < 0) {
			pr_err("vdd_lcd_1v8_s regulator enable failed\n");
			goto fail;
		}
	}
	usleep_range(3000, 5000);

	if (avdd_lcd_3v0_2v8) {
		err = regulator_enable(avdd_lcd_3v0_2v8);
		if (err < 0) {
			pr_err("avdd_lcd_3v0_2v8 regulator enable failed\n");
			goto fail;
		}
		regulator_set_voltage(avdd_lcd_3v0_2v8, 3000000, 3000000);
	}
	usleep_range(3000, 5000);

	if (vdd_lcd_bl) {
		err = regulator_enable(vdd_lcd_bl);
		if (err < 0) {
			pr_err("vdd_lcd_bl regulator enable failed\n");
			goto fail;
		}
	}

	if (vdd_lcd_bl_en) {
		err = regulator_enable(vdd_lcd_bl_en);
		if (err < 0) {
			pr_err("vdd_lcd_bl_en regulator enable failed\n");
			goto fail;
		}
	}
	usleep_range(3000, 5000);

	gpio_set_value(dsi_j_1440_810_5_8_pdata.dsi_panel_rst_gpio, 1);
	msleep(20);
	gpio_set_value(DSI_PANEL_EN_GPIO, 1);
	msleep(20);

	return 0;
fail:
	return err;
}

static struct tegra_dsi_out dsi_j_1440_810_5_8_pdata = {
	.n_data_lanes = 4,
	.controller_vs = DSI_VS_1,
	.pixel_format = TEGRA_DSI_PIXEL_FORMAT_24BIT_P,
	.refresh_rate = 60,
	.virtual_channel = TEGRA_DSI_VIRTUAL_CHANNEL_0,

	.panel_reset = DSI_PANEL_RESET,
	.power_saving_suspend = true,
	.video_data_type = TEGRA_DSI_VIDEO_TYPE_VIDEO_MODE,
	.video_clock_mode = TEGRA_DSI_VIDEO_CLOCK_CONTINUOUS,
	.video_burst_mode = TEGRA_DSI_VIDEO_NONE_BURST_MODE_WITH_SYNC_END,
	.dsi_init_cmd = dsi_j_1440_810_5_8_init_cmd,
	.n_init_cmd = ARRAY_SIZE(dsi_j_1440_810_5_8_init_cmd),
	.dsi_suspend_cmd = dsi_j_1440_810_5_8_suspend_cmd,
	.n_suspend_cmd = ARRAY_SIZE(dsi_j_1440_810_5_8_suspend_cmd),
};

static int dsi_j_1440_810_5_8_disable(void)
{
	if (vdd_lcd_bl)
		regulator_disable(vdd_lcd_bl);

	if (vdd_lcd_bl_en)
		regulator_disable(vdd_lcd_bl_en);

	if (vdd_lcd_s_1v8)
		regulator_disable(vdd_lcd_s_1v8);

	if (avdd_lcd_3v0_2v8)
		regulator_disable(avdd_lcd_3v0_2v8);

	return 0;
}

static int dsi_j_1440_810_5_8_postsuspend(void)
{
	/* TODO */
	return 0;
}

static int dsi_j_1440_810_5_8_register_bl_dev(void)
{
	int err = 0;

	err = platform_device_register(&tegra_pwfm_device);
	if (err) {
		pr_err("disp1 pwm device registration failed");
		return err;
	}

	err = platform_device_register(&dsi_j_1440_810_5_8_bl_device);
	if (err) {
		pr_err("disp1 bl device registration failed");
		return err;
	}

	err = gpio_request(dsi_j_1440_810_5_8_pdata.dsi_panel_bl_pwm_gpio,
		"panel pwm");
	if (err < 0) {
		pr_err("panel backlight pwm gpio request failed\n");
		return err;
	}
	gpio_free(dsi_j_1440_810_5_8_pdata.dsi_panel_bl_pwm_gpio);
	return err;
}

static void dsi_j_1440_810_5_8_set_disp_device
	(struct platform_device *loki_display_device)
{
	disp_device = loki_display_device;
}

static void dsi_j_1440_810_5_8_dc_out_init(struct tegra_dc_out *dc)
{
	dc->dsi = &dsi_j_1440_810_5_8_pdata;
	dc->modes = dsi_j_1440_810_5_8_modes;
	dc->n_modes = ARRAY_SIZE(dsi_j_1440_810_5_8_modes);
	dc->enable = dsi_j_1440_810_5_8_enable;
	dc->disable = dsi_j_1440_810_5_8_disable;
	dc->postsuspend = dsi_j_1440_810_5_8_postsuspend;
	dc->postpoweron = dsi_j_1440_810_5_8_postpoweron;
	dc->width = 130;
	dc->height = 74;
	dc->flags = DC_CTRL_MODE;
}

static void dsi_j_1440_810_5_8_fb_data_init(struct tegra_fb_data *fb)
{
	fb->xres = dsi_j_1440_810_5_8_modes[0].h_active;
	fb->yres = dsi_j_1440_810_5_8_modes[0].v_active;
}

static void dsi_j_1440_810_5_8_sd_settings_init
	(struct tegra_dc_sd_settings *settings)
{
	settings->bl_device_name = "pwm-backlight";
}

#ifdef CONFIG_TEGRA_DC_CMU
static void dsi_j_1440_810_5_8_cmu_init(struct tegra_dc_platform_data *pdata)
{
	pdata->cmu = &dsi_j_1440_810_5_8_cmu;
}
#endif

struct tegra_panel __initdata dsi_j_1440_810_5_8 = {
	.init_sd_settings = dsi_j_1440_810_5_8_sd_settings_init,
	.init_dc_out = dsi_j_1440_810_5_8_dc_out_init,
	.init_fb_data = dsi_j_1440_810_5_8_fb_data_init,
	.set_disp_device = dsi_j_1440_810_5_8_set_disp_device,
	.register_bl_dev = dsi_j_1440_810_5_8_register_bl_dev,
#ifdef CONFIG_TEGRA_DC_CMU
	.init_cmu_data = dsi_j_1440_810_5_8_cmu_init,
#endif
};
EXPORT_SYMBOL(dsi_j_1440_810_5_8);
