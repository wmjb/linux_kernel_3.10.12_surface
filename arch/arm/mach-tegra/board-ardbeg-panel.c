/*
 * arch/arm/mach-tegra/board-ardbeg-panel.c
 *
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
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
#include <linux/of.h>

#include <mach/irqs.h>
#include <mach/dc.h>

#include "board.h"
#include "devices.h"
#include "gpio-names.h"
#include "board-ardbeg.h"
#include "board-panel.h"
#include "common.h"
#include "iomap.h"
#include "tegra12_host1x_devices.h"


struct platform_device * __init ardbeg_host1x_init(void)
{
	struct platform_device *pdev = NULL;

#ifdef CONFIG_TEGRA_GRHOST
	pdev = tegra12_register_host1x_devices();

	if (!pdev) {
		pr_err("host1x devices registration failed\n");
		return NULL;
	}
#endif
	return pdev;
}

/* hdmi related regulators */
static struct regulator *ardbeg_hdmi_reg;
static struct regulator *ardbeg_hdmi_pll;
static struct regulator *ardbeg_hdmi_vddio;

static struct resource ardbeg_disp1_resources[] = {
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
		.start	= 0, /* Filled in by ardbeg_panel_init() */
		.end	= 0, /* Filled in by ardbeg_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "ganged_dsia_regs",
		.start	= 0, /* Filled in the panel file by init_resources() */
		.end	= 0, /* Filled in the panel file by init_resources() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "ganged_dsib_regs",
		.start	= 0, /* Filled in the panel file by init_resources() */
		.end	= 0, /* Filled in the panel file by init_resources() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "dsi_regs",
		.start	= 0, /* Filled in the panel file by init_resources() */
		.end	= 0, /* Filled in the panel file by init_resources() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "mipi_cal",
		.start	= TEGRA_MIPI_CAL_BASE,
		.end	= TEGRA_MIPI_CAL_BASE + TEGRA_MIPI_CAL_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource ardbeg_disp1_edp_resources[] = {
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
		.start	= 0, /* Filled in by ardbeg_panel_init() */
		.end	= 0, /* Filled in by ardbeg_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "mipi_cal",
		.start	= TEGRA_MIPI_CAL_BASE,
		.end	= TEGRA_MIPI_CAL_BASE + TEGRA_MIPI_CAL_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name   = "sor",
		.start  = TEGRA_SOR_BASE,
		.end    = TEGRA_SOR_BASE + TEGRA_SOR_SIZE - 1,
		.flags  = IORESOURCE_MEM,
	},
	{
		.name   = "dpaux",
		.start  = TEGRA_DPAUX_BASE,
		.end    = TEGRA_DPAUX_BASE + TEGRA_DPAUX_SIZE - 1,
		.flags  = IORESOURCE_MEM,
	},
	{
		.name	= "irq_dp",
		.start	= INT_DPAUX,
		.end	= INT_DPAUX,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct resource ardbeg_disp2_resources[] = {
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
		.start	= 0, /* Filled in by ardbeg_panel_init() */
		.end	= 0, /* Filled in by ardbeg_panel_init() */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "hdmi_regs",
		.start	= TEGRA_HDMI_BASE,
		.end	= TEGRA_HDMI_BASE + TEGRA_HDMI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};


static struct tegra_dc_sd_settings sd_settings;

static struct tegra_dc_out ardbeg_disp1_out = {
	.type		= TEGRA_DC_OUT_DSI,
	.sd_settings	= &sd_settings,
};

static int ardbeg_hdmi_enable(struct device *dev)
{
	int ret;
	if (!ardbeg_hdmi_reg) {
		ardbeg_hdmi_reg = regulator_get(dev, "avdd_hdmi");
		if (IS_ERR_OR_NULL(ardbeg_hdmi_reg)) {
			pr_err("hdmi: couldn't get regulator avdd_hdmi\n");
			ardbeg_hdmi_reg = NULL;
			return PTR_ERR(ardbeg_hdmi_reg);
		}
	}
	ret = regulator_enable(ardbeg_hdmi_reg);
	if (ret < 0) {
		pr_err("hdmi: couldn't enable regulator avdd_hdmi\n");
		return ret;
	}
	if (!ardbeg_hdmi_pll) {
		ardbeg_hdmi_pll = regulator_get(dev, "avdd_hdmi_pll");
		if (IS_ERR_OR_NULL(ardbeg_hdmi_pll)) {
			pr_err("hdmi: couldn't get regulator avdd_hdmi_pll\n");
			ardbeg_hdmi_pll = NULL;
			regulator_put(ardbeg_hdmi_reg);
			ardbeg_hdmi_reg = NULL;
			return PTR_ERR(ardbeg_hdmi_pll);
		}
	}
	ret = regulator_enable(ardbeg_hdmi_pll);
	if (ret < 0) {
		pr_err("hdmi: couldn't enable regulator avdd_hdmi_pll\n");
		return ret;
	}
	return 0;
}

static int ardbeg_hdmi_disable(void)
{
	if (ardbeg_hdmi_reg) {
		regulator_disable(ardbeg_hdmi_reg);
		regulator_put(ardbeg_hdmi_reg);
		ardbeg_hdmi_reg = NULL;
	}

	if (ardbeg_hdmi_pll) {
		regulator_disable(ardbeg_hdmi_pll);
		regulator_put(ardbeg_hdmi_pll);
		ardbeg_hdmi_pll = NULL;
	}
	return 0;
}

static int ardbeg_hdmi_postsuspend(void)
{
	if (ardbeg_hdmi_vddio) {
		regulator_disable(ardbeg_hdmi_vddio);
		regulator_put(ardbeg_hdmi_vddio);
		ardbeg_hdmi_vddio = NULL;
	}
	return 0;
}

static int ardbeg_hdmi_hotplug_init(struct device *dev)
{
	if (!ardbeg_hdmi_vddio) {
		ardbeg_hdmi_vddio = regulator_get(dev, "vdd_hdmi_5v0");
		if (WARN_ON(IS_ERR(ardbeg_hdmi_vddio))) {
			pr_err("%s: couldn't get regulator vdd_hdmi_5v0: %ld\n",
				__func__, PTR_ERR(ardbeg_hdmi_vddio));
				ardbeg_hdmi_vddio = NULL;
		} else {
			regulator_enable(ardbeg_hdmi_vddio);
		}
	}

	return 0;
}

struct tmds_config ardbeg_tmds_config[] = {
	{ /* 480p/576p / 25.2MHz/27MHz modes */
	.pclk = 27000000,
	.pll0 = 0x01003110,
	.pll1 = 0x00300F00,
	.pe_current = 0x08080808,
	.drive_current = 0x2e2e2e2e,
	.peak_current = 0x00000000,
	},
	{ /* 720p / 74.25MHz modes */
	.pclk = 74250000,
	.pll0 =  0x01003310,
	.pll1 = 0x10300F00,
	.pe_current = 0x08080808,
	.drive_current = 0x20202020,
	.peak_current = 0x00000000,
	},
	{ /* 1080p / 148.5MHz modes */
	.pclk = 148500000,
	.pll0 = 0x01003310,
	.pll1 = 0x10300F00,
	.pe_current = 0x08080808,
	.drive_current = 0x20202020,
	.peak_current = 0x00000000,
	},
	{
	.pclk = INT_MAX,
	.pll0 = 0x01003310,
	.pll1 = 0x10300F00,
	.pe_current = 0x08080808,
	.drive_current = 0x3A353536, /* lane3 needs a slightly lower current */
	.peak_current = 0x00000000,
	},
};

struct tegra_hdmi_out ardbeg_hdmi_out = {
	.tmds_config = ardbeg_tmds_config,
	.n_tmds_config = ARRAY_SIZE(ardbeg_tmds_config),
};


static struct tegra_dc_out ardbeg_disp2_out = {
	.type		= TEGRA_DC_OUT_HDMI,
	.flags		= TEGRA_DC_OUT_HOTPLUG_HIGH,
	.parent_clk	= "pll_d2",

	.dcc_bus	= 3,
	.hotplug_gpio	= ardbeg_hdmi_hpd,
	.hdmi_out	= &ardbeg_hdmi_out,

	/* TODO: update max pclk to POR */
	.max_pixclock	= KHZ2PICOS(297000),

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,

	.enable		= ardbeg_hdmi_enable,
	.disable	= ardbeg_hdmi_disable,
	.postsuspend	= ardbeg_hdmi_postsuspend,
	.hotplug_init	= ardbeg_hdmi_hotplug_init,
};

static struct tegra_fb_data ardbeg_disp1_fb_data = {
	.win		= 0,
	.bits_per_pixel = 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
};

static struct tegra_dc_platform_data ardbeg_disp1_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &ardbeg_disp1_out,
	.fb		= &ardbeg_disp1_fb_data,
	.emc_clk_rate	= 204000000,
#ifdef CONFIG_TEGRA_DC_CMU
	.cmu_enable	= 1,
#endif
};

static struct tegra_fb_data ardbeg_disp2_fb_data = {
	.win		= 0,
	.xres		= 1280,
	.yres		= 720,
	.bits_per_pixel = 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
};

static struct tegra_dc_platform_data ardbeg_disp2_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &ardbeg_disp2_out,
	.fb		= &ardbeg_disp2_fb_data,
	.emc_clk_rate	= 300000000,
};

static struct platform_device ardbeg_disp2_device = {
	.name		= "tegradc",
	.id		= 1,
	.resource	= ardbeg_disp2_resources,
	.num_resources	= ARRAY_SIZE(ardbeg_disp2_resources),
	.dev = {
		.platform_data = &ardbeg_disp2_pdata,
	},
};

static struct platform_device ardbeg_disp1_device = {
	.name		= "tegradc",
	.id		= 0,
	.resource	= ardbeg_disp1_resources,
	.num_resources	= ARRAY_SIZE(ardbeg_disp1_resources),
	.dev = {
		.platform_data = &ardbeg_disp1_pdata,
	},
};

static struct nvmap_platform_carveout ardbeg_carveouts[] = {
	[0] = {
		.name		= "iram",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_IRAM,
		.base		= TEGRA_IRAM_BASE + TEGRA_RESET_HANDLER_SIZE,
		.size		= TEGRA_IRAM_SIZE - TEGRA_RESET_HANDLER_SIZE,
	},
	[1] = {
		.name		= "generic-0",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_GENERIC,
		.base		= 0, /* Filled in by ardbeg_panel_init() */
		.size		= 0, /* Filled in by ardbeg_panel_init() */
	},
	[2] = {
		.name		= "vpr",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_VPR,
		.base		= 0, /* Filled in by ardbeg_panel_init() */
		.size		= 0, /* Filled in by ardbeg_panel_init() */
	},
};

static struct nvmap_platform_data ardbeg_nvmap_data = {
	.carveouts	= ardbeg_carveouts,
	.nr_carveouts	= ARRAY_SIZE(ardbeg_carveouts),
};
static struct platform_device ardbeg_nvmap_device  = {
	.name	= "tegra-nvmap",
	.id	= -1,
	.dev	= {
		.platform_data = &ardbeg_nvmap_data,
	},
};

static struct tegra_dc_sd_settings ardbeg_sd_settings = {
	.enable = 1, /* enabled by default. */
	.use_auto_pwm = false,
	.hw_update_delay = 0,
	.bin_width = -1,
	.aggressiveness = 5,
	.use_vid_luma = false,
	.phase_in_adjustments = 0,
	.k_limit_enable = true,
	.k_limit = 200,
	.sd_window_enable = false,
	.soft_clipping_enable = true,
	/* Low soft clipping threshold to compensate for aggressive k_limit */
	.soft_clipping_threshold = 128,
	.smooth_k_enable = false,
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
	.use_vpulse2 = true,
};

static void ardbeg_panel_select(void)
{
	struct tegra_panel *panel = NULL;
	struct board_info board;
	u8 dsi_instance;

	tegra_get_display_board_info(&board);

	switch (board.board_id) {
	case BOARD_E1639:
	case BOARD_E1813:
		panel = &dsi_s_wqxga_10_1;
		dsi_instance = DSI_INSTANCE_0;
		break;
	case BOARD_PM354:
		panel = &dsi_a_1080p_14_0;
		dsi_instance = DSI_INSTANCE_0;
		break;
	case BOARD_E1627:
		panel = &dsi_p_wuxga_10_1;
		dsi_instance = DSI_INSTANCE_0;
		break;
	case BOARD_E1549:
		panel = &dsi_lgd_wxga_7_0;
		dsi_instance = DSI_INSTANCE_0;
		break;
	case BOARD_PM363:
	case BOARD_E1824:
		panel = &edp_a_1080p_14_0;
		ardbeg_disp1_out.type = TEGRA_DC_OUT_DP;
		ardbeg_disp1_device.resource = ardbeg_disp1_edp_resources;
		ardbeg_disp1_device.num_resources =
			ARRAY_SIZE(ardbeg_disp1_edp_resources);
		break;
	case BOARD_PM366:
		panel = &lvds_c_1366_14;
		ardbeg_disp1_out.type = TEGRA_DC_OUT_LVDS;
		ardbeg_disp1_device.resource = ardbeg_disp1_edp_resources;
		ardbeg_disp1_device.num_resources =
			ARRAY_SIZE(ardbeg_disp1_edp_resources);
		break;
	default:
		panel = &dsi_p_wuxga_10_1;
		dsi_instance = DSI_INSTANCE_0;
		break;
	}

	if (panel) {
		if (panel->init_sd_settings)
			panel->init_sd_settings(&sd_settings);

		if (panel->init_dc_out) {
			panel->init_dc_out(&ardbeg_disp1_out);
			if (ardbeg_disp1_out.type == TEGRA_DC_OUT_DSI) {
				ardbeg_disp1_out.dsi->dsi_instance =
					dsi_instance;
				ardbeg_disp1_out.dsi->dsi_panel_rst_gpio =
					DSI_PANEL_RST_GPIO;
				ardbeg_disp1_out.dsi->dsi_panel_bl_pwm_gpio =
					DSI_PANEL_BL_PWM_GPIO;
				ardbeg_disp1_out.dsi->te_gpio = TEGRA_GPIO_PR6;
			}
		}

		if (panel->init_fb_data)
			panel->init_fb_data(&ardbeg_disp1_fb_data);

		if (panel->init_cmu_data)
			panel->init_cmu_data(&ardbeg_disp1_pdata);

		if (panel->set_disp_device)
			panel->set_disp_device(&ardbeg_disp1_device);

		if (ardbeg_disp1_out.type == TEGRA_DC_OUT_DSI) {
			tegra_dsi_resources_init(dsi_instance,
				ardbeg_disp1_resources,
				ARRAY_SIZE(ardbeg_disp1_resources));
		}

		if (panel->register_bl_dev)
			panel->register_bl_dev();

		if (panel->register_i2c_bridge)
			panel->register_i2c_bridge();
	}

}
int __init ardbeg_panel_init(void)
{
	int err = 0;
	struct resource __maybe_unused *res;
	struct platform_device *phost1x = NULL;

	sd_settings = ardbeg_sd_settings;

	ardbeg_panel_select();

#ifdef CONFIG_TEGRA_NVMAP
	ardbeg_carveouts[1].base = tegra_carveout_start;
	ardbeg_carveouts[1].size = tegra_carveout_size;
	ardbeg_carveouts[2].base = tegra_vpr_start;
	ardbeg_carveouts[2].size = tegra_vpr_size;

	err = platform_device_register(&ardbeg_nvmap_device);
	if (err) {
		pr_err("nvmap device registration failed\n");
		return err;
	}
#endif

	phost1x = ardbeg_host1x_init();
	if (!phost1x) {
		pr_err("host1x devices registration failed\n");
		return -EINVAL;
	}

	res = platform_get_resource_byname(&ardbeg_disp1_device,
					 IORESOURCE_MEM, "fbmem");
	res->start = tegra_fb_start;
	res->end = tegra_fb_start + tegra_fb_size - 1;

	/* Copy the bootloader fb to the fb. */
	if (tegra_bootloader_fb_size)
		__tegra_move_framebuffer(&ardbeg_nvmap_device,
				tegra_fb_start, tegra_bootloader_fb_start,
				min(tegra_fb_size, tegra_bootloader_fb_size));
	else
		__tegra_clear_framebuffer(&ardbeg_nvmap_device,
					  tegra_fb_start, tegra_fb_size);

	ardbeg_disp1_device.dev.parent = &phost1x->dev;
	err = platform_device_register(&ardbeg_disp1_device);
	if (err) {
		pr_err("disp1 device registration failed\n");
		return err;
	}

	err = tegra_init_hdmi(&ardbeg_disp2_device, phost1x);
	if (err)
		return err;

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
