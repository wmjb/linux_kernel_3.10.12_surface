/*
 * arch/arm/mach-tegra/board-dalmore.c
 *
 * Copyright (c) 2012-2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/serial_8250.h>
#include <linux/i2c.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/i2c-tegra.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/spi/spi.h>
#include <linux/spi/rm31080a_ts.h>
#include <linux/platform_data/serial-tegra.h>
#include <linux/memblock.h>
#include <linux/spi/spi-tegra.h>
#include <linux/nfc/pn544.h>
#include <linux/rfkill-gpio.h>
#include <linux/skbuff.h>
#include <linux/ti_wilink_st.h>
#include <linux/regulator/consumer.h>
#include <linux/smb349-charger.h>
#include <linux/max17048_battery.h>
#include <linux/leds.h>
#include <linux/i2c/at24.h>
#include <linux/of_platform.h>
#include <linux/edp.h>
#include <linux/usb/tegra_usb_phy.h>
#include <linux/clk/tegra.h>

#include <mach/irqs.h>
#include <mach/pinmux.h>
#include <mach/pinmux-t11.h>
#include <mach/io_dpd.h>
#include <mach/i2s.h>
#include <mach/isomgr.h>
#include <mach/tegra_asoc_pdata.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <mach/gpio-tegra.h>
#include <mach/tegra_fiq_debugger.h>
#include <linux/platform_data/tegra_usb_modem_power.h>
#include <mach/xusb.h>

#include "board-touch-raydium.h"
#include "board.h"
#include "board-common.h"
#include "clock.h"
#include "board-dalmore.h"
#include "devices.h"
#include "gpio-names.h"
#include "fuse.h"
#include "iomap.h"
#include "pm.h"
#include "pm-irq.h"
#include "common.h"
#include "tegra-board-id.h"

static struct board_info board_info, display_board_info;

#if defined(CONFIG_BT_BLUESLEEP) || defined(CONFIG_BT_BLUESLEEP_MODULE)
static struct rfkill_gpio_platform_data dalmore_bt_rfkill_pdata = {
		.name           = "bt_rfkill",
		.shutdown_gpio  = TEGRA_GPIO_PQ7,
		.reset_gpio	= TEGRA_GPIO_PQ6,
		.type           = RFKILL_TYPE_BLUETOOTH,
};

static struct platform_device dalmore_bt_rfkill_device = {
	.name = "rfkill_gpio",
	.id             = -1,
	.dev = {
		.platform_data = &dalmore_bt_rfkill_pdata,
	},
};

static struct resource dalmore_bluesleep_resources[] = {
	[0] = {
		.name = "gpio_host_wake",
			.start  = TEGRA_GPIO_PU6,
			.end    = TEGRA_GPIO_PU6,
			.flags  = IORESOURCE_IO,
	},
	[1] = {
		.name = "gpio_ext_wake",
			.start  = TEGRA_GPIO_PEE1,
			.end    = TEGRA_GPIO_PEE1,
			.flags  = IORESOURCE_IO,
	},
	[2] = {
		.name = "host_wake",
			.flags  = IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHEDGE,
	},
};

static struct platform_device dalmore_bluesleep_device = {
	.name           = "bluesleep",
	.id             = -1,
	.num_resources  = ARRAY_SIZE(dalmore_bluesleep_resources),
	.resource       = dalmore_bluesleep_resources,
};

static noinline void __init dalmore_setup_bt_rfkill(void)
{
	platform_device_register(&dalmore_bt_rfkill_device);
}

static noinline void __init dalmore_setup_bluesleep(void)
{
	dalmore_bluesleep_resources[2].start =
		dalmore_bluesleep_resources[2].end =
			gpio_to_irq(TEGRA_GPIO_PU6);
	platform_device_register(&dalmore_bluesleep_device);
	return;
}
#elif defined(CONFIG_BLUEDROID_PM) || defined(CONFIG_BLUEDROID_PM_MODULE)
static struct resource dalmore_bluedroid_pm_resources[] = {
	[0] = {
		.name   = "shutdown_gpio",
		.start  = TEGRA_GPIO_PQ7,
		.end    = TEGRA_GPIO_PQ7,
		.flags  = IORESOURCE_IO,
	},
	[1] = {
		.name = "host_wake",
		.flags  = IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHEDGE,
	},
	[2] = {
		.name = "gpio_ext_wake",
		.start  = TEGRA_GPIO_PEE1,
		.end    = TEGRA_GPIO_PEE1,
		.flags  = IORESOURCE_IO,
	},
	[3] = {
		.name = "gpio_host_wake",
		.start  = TEGRA_GPIO_PU6,
		.end    = TEGRA_GPIO_PU6,
		.flags  = IORESOURCE_IO,
	},
	[4] = {
		.name = "reset_gpio",
		.start  = TEGRA_GPIO_PQ6,
		.end    = TEGRA_GPIO_PQ6,
		.flags  = IORESOURCE_IO,
	},
};

static struct platform_device dalmore_bluedroid_pm_device = {
	.name = "bluedroid_pm",
	.id             = 0,
	.num_resources  = ARRAY_SIZE(dalmore_bluedroid_pm_resources),
	.resource       = dalmore_bluedroid_pm_resources,
};

static noinline void __init dalmore_setup_bluedroid_pm(void)
{
	dalmore_bluedroid_pm_resources[1].start =
		dalmore_bluedroid_pm_resources[1].end =
				gpio_to_irq(TEGRA_GPIO_PU6);
	platform_device_register(&dalmore_bluedroid_pm_device);
}
#endif

static __initdata struct tegra_clk_init_table dalmore_clk_init_table[] = {
	/* name		parent		rate		enabled */
	{ "pll_m",	NULL,		0,		false},
	{ "hda",	"pll_p",	108000000,	false},
	{ "hda2codec_2x", "pll_p",	48000000,	false},
	{ "pwm",	"pll_p",	3187500,	false},
	{ "blink",	"clk_32k",	32768,		true},
	{ "i2s1",	"pll_a_out0",	0,		false},
	{ "i2s3",	"pll_a_out0",	0,		false},
	{ "i2s4",	"pll_a_out0",	0,		false},
	{ "spdif_out",	"pll_a_out0",	0,		false},
	{ "d_audio",	"clk_m",	12000000,	false},
	{ "dam0",	"clk_m",	12000000,	false},
	{ "dam1",	"clk_m",	12000000,	false},
	{ "dam2",	"clk_m",	12000000,	false},
	{ "audio1",	"i2s1_sync",	0,		false},
	{ "audio3",	"i2s3_sync",	0,		false},
	/* Setting vi_sensor-clk to true for validation purpose, will imapact
	 * power, later set to be false.*/
	{ "vi_sensor",	"pll_p",	150000000,	false},
	{ "cilab",	"pll_p",	150000000,	false},
	{ "cilcd",	"pll_p",	150000000,	false},
	{ "cile",	"pll_p",	150000000,	false},
	{ "i2c1",	"pll_p",	3200000,	false},
	{ "i2c2",	"pll_p",	3200000,	false},
	{ "i2c3",	"pll_p",	3200000,	false},
	{ "i2c4",	"pll_p",	3200000,	false},
	{ "i2c5",	"pll_p",	3200000,	false},
	{ "sbc1",	"pll_p",	25000000,	false},
	{ "sbc2",	"pll_p",	25000000,	false},
	{ "sbc3",	"pll_p",	25000000,	false},
	{ "sbc4",	"pll_p",	25000000,	false},
	{ "sbc5",	"pll_p",	25000000,	false},
	{ "sbc6",	"pll_p",	25000000,	false},
	{ "uarta",	"pll_p",	408000000,	false},
	{ "uartb",	"pll_p",	408000000,	false},
	{ "uartc",	"pll_p",	408000000,	false},
	{ "uartd",	"pll_p",	408000000,	false},
	{ NULL,		NULL,		0,		0},
};

#ifndef CONFIG_USE_OF
static struct tegra_i2c_platform_data dalmore_i2c1_platform_data = {
	.bus_clk_rate	= 100000,
	.scl_gpio	= TEGRA_GPIO_I2C1_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C1_SDA,
};

static struct tegra_i2c_platform_data dalmore_i2c2_platform_data = {
	.bus_clk_rate	= 100000,
	.is_clkon_always = true,
	.scl_gpio	= TEGRA_GPIO_I2C2_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C2_SDA,
};

static struct tegra_i2c_platform_data dalmore_i2c3_platform_data = {
	.bus_clk_rate	= 400000,
	.scl_gpio	= TEGRA_GPIO_I2C3_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C3_SDA,
};

static struct tegra_i2c_platform_data dalmore_i2c4_platform_data = {
	.bus_clk_rate	= 10000,
	.scl_gpio	= TEGRA_GPIO_I2C4_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C4_SDA,
};

static struct tegra_i2c_platform_data dalmore_i2c5_platform_data = {
	.bus_clk_rate	= 400000,
	.scl_gpio	= TEGRA_GPIO_I2C5_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C5_SDA,
	.needs_cl_dvfs_clock = true,
};
#endif

static struct i2c_board_info __initdata rt5640_board_info = {
	I2C_BOARD_INFO("rt5640", 0x1c),
};

static struct pn544_i2c_platform_data nfc_pdata = {
	.irq_gpio = TEGRA_GPIO_PW2,
	.ven_gpio = TEGRA_GPIO_PQ3,
	.firm_gpio = TEGRA_GPIO_PH0,
};

static struct i2c_board_info __initdata nfc_board_info = {
	I2C_BOARD_INFO("pn544", 0x28),
	.platform_data = &nfc_pdata,
};

static void dalmore_i2c_init(void)
{
#ifndef CONFIG_USE_OF
	tegra11_i2c_device1.dev.platform_data = &dalmore_i2c1_platform_data;
	tegra11_i2c_device2.dev.platform_data = &dalmore_i2c2_platform_data;
	tegra11_i2c_device3.dev.platform_data = &dalmore_i2c3_platform_data;
	tegra11_i2c_device4.dev.platform_data = &dalmore_i2c4_platform_data;
	tegra11_i2c_device5.dev.platform_data = &dalmore_i2c5_platform_data;

	platform_device_register(&tegra11_i2c_device5);
	platform_device_register(&tegra11_i2c_device4);
	platform_device_register(&tegra11_i2c_device3);
	platform_device_register(&tegra11_i2c_device2);
	platform_device_register(&tegra11_i2c_device1);
#endif
	nfc_board_info.irq = gpio_to_irq(TEGRA_GPIO_PW2);
	i2c_register_board_info(0, &nfc_board_info, 1);
	i2c_register_board_info(0, &rt5640_board_info, 1);
}

static struct tegra_serial_platform_data dalmore_uartd_pdata = {
	.dma_req_selector = 19,
	.modem_interrupt = false,
};

static void __init dalmore_uart_init(void)
{
	int debug_port_id;

	/* Register low speed only if it is selected */
	if (!is_tegra_debug_uartport_hs()) {
		debug_port_id = uart_console_debug_init(3);
		if (debug_port_id < 0)
			return;

		platform_device_register(uart_console_debug_device);

	} else {
		tegra_uartd_device.dev.platform_data = &dalmore_uartd_pdata;
		platform_device_register(&tegra_uartd_device);
	}

}

static struct resource tegra_rtc_resources[] = {
	[0] = {
		.start = TEGRA_RTC_BASE,
		.end = TEGRA_RTC_BASE + TEGRA_RTC_SIZE - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = INT_RTC,
		.end = INT_RTC,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device tegra_rtc_device = {
	.name = "tegra_rtc",
	.id   = -1,
	.resource = tegra_rtc_resources,
	.num_resources = ARRAY_SIZE(tegra_rtc_resources),
};

static struct tegra_asoc_platform_data dalmore_audio_pdata = {
	.gpio_spkr_en		= TEGRA_GPIO_SPKR_EN,
	.gpio_hp_det		= TEGRA_GPIO_HP_DET,
	.gpio_hp_mute		= -1,
	.gpio_int_mic_en	= TEGRA_GPIO_INT_MIC_EN,
	.gpio_ext_mic_en	= TEGRA_GPIO_EXT_MIC_EN,
	.gpio_ldo1_en		= TEGRA_GPIO_LDO1_EN,
	.gpio_codec1 = TEGRA_GPIO_CODEC1_EN,
	.gpio_codec2 = TEGRA_GPIO_CODEC2_EN,
	.gpio_codec3 = TEGRA_GPIO_CODEC3_EN,
	.i2s_param[HIFI_CODEC]	= {
		.audio_port_id	= 1,
		.is_i2s_master	= 1,
		.i2s_mode	= TEGRA_DAIFMT_I2S,
	},
	.i2s_param[BT_SCO]	= {
		.audio_port_id	= 3,
		.is_i2s_master	= 1,
		.i2s_mode	= TEGRA_DAIFMT_DSP_A,
	},
};

static struct platform_device dalmore_audio_device = {
	.name	= "tegra-snd-rt5640",
	.id	= 0,
	.dev	= {
		.platform_data = &dalmore_audio_pdata,
	},
};

static struct platform_device *dalmore_devices[] __initdata = {
	&tegra_pmu_device,
	&tegra_rtc_device,
	&tegra_udc_device,
#if defined(CONFIG_TEGRA_AVP)
	&tegra_avp_device,
#endif
#if defined(CONFIG_CRYPTO_DEV_TEGRA_SE)
	&tegra11_se_device,
#endif
	&tegra_ahub_device,
	&tegra_dam_device0,
	&tegra_dam_device1,
	&tegra_dam_device2,
	&tegra_i2s_device1,
	&tegra_i2s_device3,
	&tegra_i2s_device4,
	&tegra_spdif_device,
	&spdif_dit_device,
	&bluetooth_dit_device,
	&tegra_pcm_device,
	&dalmore_audio_device,
	&tegra_hda_device,
#if defined(CONFIG_TEGRA_CEC_SUPPORT)
	&tegra_cec_device,
#endif
#if defined(CONFIG_CRYPTO_DEV_TEGRA_AES)
	&tegra_aes_device,
#endif
};

#ifdef CONFIG_USB_SUPPORT
static struct tegra_usb_platform_data tegra_udc_pdata = {
	.port_otg = true,
	.has_hostpc = true,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.unaligned_dma_buf_supported = false,
	.op_mode = TEGRA_USB_OPMODE_DEVICE,
	.u_data.dev = {
		.vbus_pmu_irq = 0,
		.vbus_gpio = -1,
		.charging_supported = false,
		.remote_wakeup_supported = false,
	},
	.u_cfg.utmi = {
		.hssync_start_delay = 0,
		.elastic_limit = 16,
		.idle_wait_delay = 17,
		.term_range_adj = 6,
		.xcvr_setup = 8,
		.xcvr_lsfslew = 0,
		.xcvr_lsrslew = 3,
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
	},
};

static struct tegra_usb_platform_data tegra_ehci1_utmi_pdata = {
	.port_otg = true,
	.has_hostpc = true,
	.unaligned_dma_buf_supported = false,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.op_mode = TEGRA_USB_OPMODE_HOST,
	.u_data.host = {
		.vbus_gpio = -1,
		.hot_plug = false,
		.remote_wakeup_supported = true,
		.power_off_on_suspend = true,
	},
	.u_cfg.utmi = {
		.hssync_start_delay = 0,
		.elastic_limit = 16,
		.idle_wait_delay = 17,
		.term_range_adj = 6,
		.xcvr_setup = 15,
		.xcvr_lsfslew = 0,
		.xcvr_lsrslew = 3,
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
		.vbus_oc_map = 0x4,
	},
};

static struct tegra_usb_platform_data tegra_ehci3_utmi_pdata = {
	.port_otg = false,
	.has_hostpc = true,
	.unaligned_dma_buf_supported = false,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.op_mode = TEGRA_USB_OPMODE_HOST,
	.u_data.host = {
		.vbus_gpio = -1,
		.hot_plug = false,
		.remote_wakeup_supported = true,
		.power_off_on_suspend = true,
	},
	.u_cfg.utmi = {
	.hssync_start_delay = 0,
		.elastic_limit = 16,
		.idle_wait_delay = 17,
		.term_range_adj = 6,
		.xcvr_setup = 8,
		.xcvr_lsfslew = 0,
		.xcvr_lsrslew = 3,
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
		.vbus_oc_map = 0x5,
	},
};

static struct tegra_usb_otg_data tegra_otg_pdata = {
	.ehci_device = &tegra_ehci1_device,
	.ehci_pdata = &tegra_ehci1_utmi_pdata,
};

static void dalmore_usb_init(void)
{
	int usb_port_owner_info = tegra_get_usb_port_owner_info();

	/* Set USB wake sources for dalmore */
	tegra_set_usb_wake_source();

	if (!(usb_port_owner_info & UTMI1_PORT_OWNER_XUSB)) {
		tegra_otg_device.dev.platform_data = &tegra_otg_pdata;
		platform_device_register(&tegra_otg_device);
		/* Setup the udc platform data */
		tegra_udc_device.dev.platform_data = &tegra_udc_pdata;
	}

	if (!(usb_port_owner_info & UTMI2_PORT_OWNER_XUSB)) {
		tegra_ehci3_device.dev.platform_data = &tegra_ehci3_utmi_pdata;
		platform_device_register(&tegra_ehci3_device);
	}
}

static struct tegra_xusb_board_data xusb_bdata = {
	.portmap = TEGRA_XUSB_SS_P0 | TEGRA_XUSB_USB2_P1,
	/* ss_portmap[0:3] = SS0 map, ss_portmap[4:7] = SS1 map */
	.ss_portmap = (TEGRA_XUSB_SS_PORT_MAP_USB2_P1 << 0),
	.uses_external_pmic = false,
	.supply = {
		.utmi_vbuses = {
			NULL, "usb_vbus", NULL
		},
		.s3p3v = "hvdd_usb",
		.s1p8v = "avdd_usb_pll",
		.vddio_hsic = "vddio_hsic",
		.s1p05v = "avddio_usb",
	},
};

static void dalmore_xusb_init(void)
{
	int usb_port_owner_info = tegra_get_usb_port_owner_info();

	if (usb_port_owner_info & UTMI2_PORT_OWNER_XUSB)
		tegra_xusb_init(&xusb_bdata);
}

static struct gpio modem_gpios[] = { /* Nemo modem */
	{MODEM_EN, GPIOF_OUT_INIT_HIGH, "MODEM EN"},
	{MDM_RST, GPIOF_OUT_INIT_LOW, "MODEM RESET"},
};

static struct tegra_usb_platform_data tegra_ehci2_hsic_baseband_pdata = {
	.port_otg = false,
	.has_hostpc = true,
	.unaligned_dma_buf_supported = false,
	.phy_intf = TEGRA_USB_PHY_INTF_HSIC,
	.op_mode = TEGRA_USB_OPMODE_HOST,
	.u_data.host = {
		.vbus_gpio = -1,
		.hot_plug = false,
		.remote_wakeup_supported = true,
		.power_off_on_suspend = true,
	},
};

static int baseband_init(void)
{
	int ret;

	ret = gpio_request_array(modem_gpios, ARRAY_SIZE(modem_gpios));
	if (ret) {
		pr_warn("%s:gpio request failed\n", __func__);
		return ret;
	}

	/* enable pull-down for MDM_COLD_BOOT */
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_COL5,
				    TEGRA_PUPD_PULL_DOWN);

	/* export GPIO for user space access through sysfs */
	gpio_export(MDM_RST, false);

	return 0;
}

static const struct tegra_modem_operations baseband_operations = {
	.init = baseband_init,
};

static struct tegra_usb_modem_power_platform_data baseband_pdata = {
	.ops = &baseband_operations,
	.regulator_name = "vdd_modem_3v3",
	.wake_gpio = -1,
	.boot_gpio = MDM_COLDBOOT,
	.boot_irq_flags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
	.autosuspend_delay = 2000,
	.short_autosuspend_delay = 50,
	.tegra_ehci_device = &tegra_ehci2_device,
	.tegra_ehci_pdata = &tegra_ehci2_hsic_baseband_pdata,
};

static struct platform_device icera_nemo_device = {
	.name = "tegra_usb_modem_power",
	.id = -1,
	.dev = {
		.platform_data = &baseband_pdata,
	},
};

static void dalmore_modem_init(void)
{
	int modem_id = tegra_get_modem_id();
	int usb_port_owner_info = tegra_get_usb_port_owner_info();
	switch (modem_id) {
	case TEGRA_BB_NEMO: /* on board i500 HSIC */
		if (!(usb_port_owner_info & HSIC1_PORT_OWNER_XUSB)) {
			platform_device_register(&icera_nemo_device);
		}
		break;
	}
}

#else
static void dalmore_usb_init(void) { }
static void dalmore_modem_init(void) { }
static void dalmore_xusb_init(void) { }
#endif

static void dalmore_audio_init(void)
{
	dalmore_audio_pdata.codec_name = "rt5640.0-001c";
	dalmore_audio_pdata.codec_dai_name = "rt5640-aif1";
}

#ifndef CONFIG_USE_OF
static struct platform_device *dalmore_spi_devices[] __initdata = {
        &tegra11_spi_device4,
};

static struct tegra_spi_platform_data dalmore_spi_pdata = {
	.dma_req_sel		= 0,
	.spi_max_frequency	= 25000000,
	.clock_always_on	= false,
};

static void __init dalmore_spi_init(void)
{
	tegra11_spi_device4.dev.platform_data = &dalmore_spi_pdata;
        platform_add_devices(dalmore_spi_devices,
                                ARRAY_SIZE(dalmore_spi_devices));
}
#else
static void __init dalmore_spi_init(void)
{
}
#endif

static __initdata struct tegra_clk_init_table touch_clk_init_table[] = {
	/* name         parent          rate            enabled */
	{ "extern2",    "pll_p",        41000000,       false},
	{ "clk_out_2",  "extern2",      40800000,       false},
	{ NULL,         NULL,           0,              0},
};

struct rm_spi_ts_platform_data rm31080ts_dalmore_data = {
	.gpio_reset = TOUCH_GPIO_RST_RAYDIUM_SPI,
	.config = 0,
	.platform_id = RM_PLATFORM_D010,
	.name_of_clock = "clk_out_2",
	.name_of_clock_con = "extern2",
};

static struct tegra_spi_device_controller_data dev_cdata = {
	.rx_clk_tap_delay = 16,
	.tx_clk_tap_delay = 16,
};

struct spi_board_info rm31080a_dalmore_spi_board[1] = {
	{
	 .modalias = "rm_ts_spidev",
	 .bus_num = 3,
	 .chip_select = 2,
	 .max_speed_hz = 12 * 1000 * 1000,
	 .mode = SPI_MODE_0,
	 .controller_data = &dev_cdata,
	 .platform_data = &rm31080ts_dalmore_data,
	 },
};

static int __init dalmore_touch_init(void)
{
	tegra_clk_init_from_table(touch_clk_init_table);
	if (display_board_info.board_id == BOARD_E1582)
		rm31080ts_dalmore_data.platform_id = RM_PLATFORM_P005;
	else
		rm31080ts_dalmore_data.platform_id = RM_PLATFORM_D010;
	mdelay(20);
	rm31080a_dalmore_spi_board[0].irq = gpio_to_irq(TOUCH_GPIO_IRQ_RAYDIUM_SPI);
	touch_init_raydium(TOUCH_GPIO_IRQ_RAYDIUM_SPI,
				TOUCH_GPIO_RST_RAYDIUM_SPI,
				&rm31080ts_dalmore_data,
				&rm31080a_dalmore_spi_board[0],
				ARRAY_SIZE(rm31080a_dalmore_spi_board));
	return 0;
}

#ifdef CONFIG_USE_OF
struct of_dev_auxdata dalmore_auxdata_lookup[] __initdata = {
	OF_DEV_AUXDATA("nvidia,tegra114-sdhci", 0x78000600, "sdhci-tegra.3",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-sdhci", 0x78000400, "sdhci-tegra.2",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-sdhci", 0x78000000, "sdhci-tegra.0",
				&dalmore_tegra_sdhci_platform_data0),
	OF_DEV_AUXDATA("nvidia,tegra114-camera", 0x0, "tegra_camera",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-host1x", TEGRA_HOST1X_BASE, "host1x",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-gr3d", TEGRA_GR3D_BASE, "gr3d",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-gr2d", TEGRA_GR2D_BASE, "gr2d",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-msenc", TEGRA_MSENC_BASE, "msenc",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-vi", TEGRA_VI_BASE, "vi",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-isp", TEGRA_ISP_BASE, "isp",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-tsec", TEGRA_TSEC_BASE, "tsec",
				NULL),

	OF_DEV_AUXDATA("nvidia,tegra114-i2c", 0x7000c000, "tegra11-i2c.0",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-i2c", 0x7000c400, "tegra11-i2c.1",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-i2c", 0x7000c500, "tegra11-i2c.2",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-i2c", 0x7000c700, "tegra11-i2c.3",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-i2c", 0x7000d000, "tegra11-i2c.4",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-spi", 0x7000d400, "spi-tegra114.0",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-spi", 0x7000d600, "spi-tegra114.1",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-spi", 0x7000d800, "spi-tegra114.2",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-spi", 0x7000da00, "spi-tegra114.3",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-spi", 0x7000dc00, "spi-tegra114.4",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-spi", 0x7000de00, "spi-tegra114.5",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-apbdma", 0x6000a000, "tegra-apbdma",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-hsuart", 0x70006000, "serial-tegra.0",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-hsuart", 0x70006040, "serial-tegra.1",
				NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-hsuart", 0x70006200, "serial-tegra.2",
				NULL),
	{}
};
#endif

static void __init tegra_dalmore_early_init(void)
{
	tegra_clk_init_from_table(dalmore_clk_init_table);
	tegra_clk_verify_parents();
	tegra_soc_device_init("dalmore");
}

static void __init tegra_dalmore_late_init(void)
{
	platform_device_register(&tegra_pinmux_device);
	dalmore_pinmux_init();
	dalmore_i2c_init();
	dalmore_spi_init();
	dalmore_usb_init();
	dalmore_xusb_init();
	dalmore_uart_init();
	dalmore_audio_init();
	platform_add_devices(dalmore_devices, ARRAY_SIZE(dalmore_devices));
	//tegra_ram_console_debug_init();
	tegra_io_dpd_init();
	dalmore_regulator_init();
	dalmore_sdhci_init();
	dalmore_suspend_init();
	dalmore_emc_init();
	dalmore_edp_init();
	isomgr_init();
	dalmore_touch_init();
	if (board_info.board_id == BOARD_E1582)
		roth_panel_init(board_info.board_id);
	else
		dalmore_panel_init();
	dalmore_kbc_init();
	dalmore_pmon_init();
#if defined(CONFIG_BT_BLUESLEEP) || defined(CONFIG_BT_BLUESLEEP_MODULE)
	dalmore_setup_bluesleep();
	dalmore_setup_bt_rfkill();
#elif defined(CONFIG_BLUEDROID_PM) || defined(CONFIG_BLUEDROID_PM_MODULE)
	dalmore_setup_bluedroid_pm();
#endif
	dalmore_modem_init();
#ifdef CONFIG_TEGRA_WDT_RECOVERY
	tegra_wdt_recovery_init();
#endif
	tegra_serial_debug_init(TEGRA_UARTD_BASE, INT_WDT_CPU, NULL, -1, -1);
	dalmore_sensors_init();
	dalmore_soctherm_init();
	tegra_register_fuse();
}

static void __init dalmore_ramconsole_reserve(unsigned long size)
{
	tegra_ram_console_debug_reserve(SZ_1M);
}

static void __init tegra_dalmore_dt_init(void)
{
	tegra_get_board_info(&board_info);
	tegra_get_display_board_info(&display_board_info);

	tegra_dalmore_early_init();

	of_platform_populate(NULL,
		of_default_bus_match_table, dalmore_auxdata_lookup,
		&platform_bus);

	tegra_dalmore_late_init();
}

static void __init tegra_dalmore_reserve(void)
{
#if defined(CONFIG_NVMAP_CONVERT_CARVEOUT_TO_IOVMM)
	/* 1920*1200*4*2 = 18432000 bytes */
	tegra_reserve(0, SZ_16M + SZ_2M, SZ_16M);
#else
	tegra_reserve(SZ_512M, SZ_16M + SZ_2M, SZ_4M);
#endif
	dalmore_ramconsole_reserve(SZ_1M);
}

static const char * const dalmore_dt_board_compat[] = {
	"nvidia,dalmore",
	NULL
};

MACHINE_START(DALMORE, "dalmore")
	.atag_offset	= 0x100,
	.smp		= smp_ops(tegra_smp_ops),
	.map_io		= tegra_map_common_io,
	.reserve	= tegra_dalmore_reserve,
	.init_early	= tegra11x_init_early,
	.init_irq	= tegra_dt_init_irq,
	.init_time	= tegra_init_timer,
	.init_machine	= tegra_dalmore_dt_init,
	.restart	= tegra_assert_system_reset,
	.dt_compat	= dalmore_dt_board_compat,
	.init_late	= tegra_init_late,
MACHINE_END
