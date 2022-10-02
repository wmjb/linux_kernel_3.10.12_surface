/*
 * arch/arm/mach-tegra/board-vcm30_t124.c
 *
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/platform_device.h>
#include <linux/i2c-tegra.h>
#include <linux/i2c.h>
#include <linux/platform_data/serial-tegra.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/platform_data/tegra_nor.h>
#include <linux/platform_data/tegra_ahci.h>
#include <linux/spi/spi-tegra.h>
#include <linux/of_platform.h>
#include <linux/kernel.h>

#include <mach/tegra_asoc_pdata.h>
#include <mach/pci.h>
#include <mach/io_dpd.h>
#include <asm/mach/arch.h>
#include <mach/isomgr.h>

#include "iomap.h"
#include "board.h"
#include "clock.h"
#include "board-vcm30_t124.h"
#include "devices.h"
#include "board-common.h"
#include "common.h"

#include <asm/mach-types.h>

static struct board_info board_info, display_board_info;

static __initdata struct tegra_clk_init_table vcm30_t124_clk_init_table[] = {
	/* name		parent		rate		enabled (always on)*/
	{ "pll_m",	NULL,		0,		false},
	{ "hda",	"pll_p",	108000000,	false},
	{ "hda2codec_2x", "pll_p",	48000000,	false},
	{ "pwm",	"pll_p",	3187500,	false},
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
	{ "vi_sensor",	"pll_p",	150000000,	false},
	{ "vi_sensor2",	"pll_p",	150000000,	false},
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
	{ "nor",	"pll_p",	102000000,	true},
	{ NULL,		NULL,		0,		0},
};

static struct tegra_i2c_platform_data vcm30_t124_i2c1_platform_data = {
	.bus_clk_rate	= 100000,
	.scl_gpio	= TEGRA_GPIO_I2C1_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C1_SDA,
};

static struct tegra_i2c_platform_data vcm30_t124_i2c2_platform_data = {
	.bus_clk_rate	= 100000,
	.is_clkon_always = true,
	.scl_gpio	= TEGRA_GPIO_I2C2_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C2_SDA,
};

static struct tegra_i2c_platform_data vcm30_t124_i2c3_platform_data = {
	.bus_clk_rate	= 400000,
	.scl_gpio	= TEGRA_GPIO_I2C3_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C3_SDA,
};

static struct tegra_i2c_platform_data vcm30_t124_i2c4_platform_data = {
	.bus_clk_rate	= 10000,
	.scl_gpio	= TEGRA_GPIO_I2C4_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C4_SDA,
};

static struct tegra_i2c_platform_data vcm30_t124_i2c5_platform_data = {
	.bus_clk_rate	= 400000,
	.scl_gpio	= TEGRA_GPIO_I2C5_SCL,
	.sda_gpio	= TEGRA_GPIO_I2C5_SDA,
};

static struct tegra_nor_platform_data vcm30_t124_nor_data = {
	.flash = {
		.map_name = "cfi_probe",
		.width = 2,
	},
	.chip_parms = {
		.MuxMode = NorMuxMode_ADNonMux,
		.ReadMode = NorReadMode_Page,
		.PageLength = NorPageLength_8Word,
		.ReadyActive = NorReadyActive_WithData,
		/* FIXME: Need to use characterized value */
		.timing_default = {
			.timing0 = 0x30300273,
			.timing1 = 0x00030302,
		},
		.timing_read = {
			.timing0 = 0x30300273,
			.timing1 = 0x00030302,
		},
	},
};

static struct cs_info vcm30_t124_cs_info[] = {
	{
		.cs = CS_0,
		.num_cs_gpio = 0,
		.virt = IO_ADDRESS(TEGRA_NOR_FLASH_BASE),
		.size = SZ_64M,
		.phys = TEGRA_NOR_FLASH_BASE,
	},
};

static void vcm30_t124_nor_init(void)
{
	tegra_nor_device.resource[2].end = TEGRA_NOR_FLASH_BASE + SZ_64M - 1;

	vcm30_t124_nor_data.info.cs = kzalloc(sizeof(struct cs_info) *
					ARRAY_SIZE(vcm30_t124_cs_info),
					GFP_KERNEL);
        if (!vcm30_t124_nor_data.info.cs)
                BUG();

        vcm30_t124_nor_data.info.num_chips = ARRAY_SIZE(vcm30_t124_cs_info);

        memcpy(vcm30_t124_nor_data.info.cs, vcm30_t124_cs_info,
                                sizeof(struct cs_info) * ARRAY_SIZE(vcm30_t124_cs_info));

	tegra_nor_device.dev.platform_data = &vcm30_t124_nor_data;
	platform_device_register(&tegra_nor_device);
}


static void vcm30_t124_i2c_init(void)
{
	struct board_info board_info;
	tegra_get_board_info(&board_info);
	/* T124 does not use device tree as of now */
	tegra12_i2c_device1.dev.platform_data = &vcm30_t124_i2c1_platform_data;
	tegra12_i2c_device2.dev.platform_data = &vcm30_t124_i2c2_platform_data;
	tegra12_i2c_device3.dev.platform_data = &vcm30_t124_i2c3_platform_data;
	tegra12_i2c_device4.dev.platform_data = &vcm30_t124_i2c4_platform_data;
	tegra12_i2c_device5.dev.platform_data = &vcm30_t124_i2c5_platform_data;

	platform_device_register(&tegra12_i2c_device5);
	platform_device_register(&tegra12_i2c_device4);
	platform_device_register(&tegra12_i2c_device3);
	platform_device_register(&tegra12_i2c_device2);
	platform_device_register(&tegra12_i2c_device1);
}

/* Register debug UART in old fashion and use DT for all others */
#ifndef CONFIG_USE_OF
static struct platform_device *vcm30_t124_uart_devices[] __initdata = {
	&tegra_uarta_device,
	&tegra_uartb_device,
	&tegra_uartd_device,
};

static struct tegra_serial_platform_data vcm30_t124_uarta_pdata = {
	.dma_req_selector = 8,
	.modem_interrupt = false,
};

static struct tegra_serial_platform_data vcm30_t124_uartb_pdata = {
	.dma_req_selector = 9,
	.modem_interrupt = false,
};

static struct tegra_serial_platform_data vcm30_t124_uartd_pdata = {
	.dma_req_selector = 19,
	.modem_interrupt = false,
};
#endif

static struct tegra_serial_platform_data vcm30_t124_uartc_pdata = {
	.dma_req_selector = 10,
	.modem_interrupt = false,
};

static void __init vcm30_t124_uart_init(void)
{
	int debug_port_id;

#ifndef CONFIG_USE_OF
	tegra_uarta_device.dev.platform_data = &vcm30_t124_uarta_pdata;
	tegra_uartb_device.dev.platform_data = &vcm30_t124_uartb_pdata;
	tegra_uartd_device.dev.platform_data = &vcm30_t124_uartd_pdata;
	platform_add_devices(vcm30_t124_uart_devices,
			ARRAY_SIZE(vcm30_t124_uart_devices));
#endif
	tegra_uartc_device.dev.platform_data = &vcm30_t124_uartc_pdata;
	if (!is_tegra_debug_uartport_hs()) {
		debug_port_id = uart_console_debug_init(2);
		if (debug_port_id < 0)
			return;

		platform_device_register(uart_console_debug_device);
	} else {
		tegra_uartc_device.dev.platform_data = &vcm30_t124_uartc_pdata;
		platform_device_register(&tegra_uartc_device);
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

static struct tegra_pci_platform_data vcm30_t124_pcie_platform_data = {
	.port_status[0]	= 1,
	.port_status[1]	= 1,
	.use_dock_detect	= 1,
	.gpio	= TEGRA_GPIO_PO1,
	.gpio_x1_slot	= PMU_TCA6416_GPIO(12),
};

static void vcm30_t124_pcie_init(void)
{
/* FIXME: Check this for VCM30_T124 */
#if 0
	struct board_info board_info;
	/* root port 1(x1 slot) is supported only on of ERS-S board */
	laguna_pcie_platform_data.port_status[1] = 0;

	tegra_pci_device.dev.platform_data = &laguna_pcie_platform_data;
	platform_device_register(&tegra_pci_device);
#endif
}

#ifdef CONFIG_SATA_AHCI_TEGRA
static struct tegra_ahci_platform_data ahci_plat_data = {
        .gen2_rx_eq = 7,
};

static void vcm30_t124_sata_init(void)
{
        tegra_sata_device.dev.platform_data = &ahci_plat_data;
        platform_device_register(&tegra_sata_device);
}
#else
static void vcm30_t124_sata_init(void) { }
#endif

/* FIXME: Check which devices are needed from the below list */
static struct platform_device *vcm30_t124_devices[] __initdata = {
	&tegra_pmu_device,
	&tegra_rtc_device,
	&tegra_udc_device,
#if defined(CONFIG_TEGRA_AVP)
	&tegra_avp_device,
#endif
#if defined(CONFIG_CRYPTO_DEV_TEGRA_SE)
	&tegra12_se_device,
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
	&tegra_hda_device,
};

static struct tegra_usb_platform_data tegra_ehci1_utmi_pdata = {
	.port_otg = true,
	.has_hostpc = true,
	.unaligned_dma_buf_supported = true,
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
		.xcvr_lsfslew = 2,
		.xcvr_lsrslew = 2,
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
		.vbus_oc_map = 0x4,
	},
};

static struct tegra_usb_platform_data tegra_ehci2_utmi_pdata = {
	.port_otg = false,
	.has_hostpc = true,
	.unaligned_dma_buf_supported = true,
	.phy_intf = TEGRA_USB_PHY_INTF_UTMI,
	.op_mode = TEGRA_USB_OPMODE_HOST,
	.u_data.host = {
		/* FIXME: Set this only for E1855. */
		.vbus_gpio = TEGRA_GPIO_PN5,
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
		.xcvr_lsfslew = 2,
		.xcvr_lsrslew = 2,
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
		.vbus_oc_map = 0x5,
	},
};

static struct tegra_usb_platform_data tegra_ehci3_utmi_pdata = {
	.port_otg = false,
	.has_hostpc = true,
	.unaligned_dma_buf_supported = true,
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
		.xcvr_lsfslew = 2,
		.xcvr_lsrslew = 2,
		.xcvr_setup_offset = 0,
		.xcvr_use_fuses = 1,
		.vbus_oc_map = 0x5,
	},
};

static struct tegra_usb_otg_data tegra_otg_pdata = {
	.ehci_device = &tegra_ehci1_device,
	.ehci_pdata = &tegra_ehci1_utmi_pdata,
};

static void vcm30_t124_usb_init(void)
{
	int usb_port_owner_info = tegra_get_usb_port_owner_info();

	if (!(usb_port_owner_info & UTMI2_PORT_OWNER_XUSB)) {
		tegra_ehci2_device.dev.platform_data = &tegra_ehci2_utmi_pdata;
		platform_device_register(&tegra_ehci2_device);
	}

	if (!(usb_port_owner_info & UTMI3_PORT_OWNER_XUSB)) {
		tegra_ehci3_device.dev.platform_data = &tegra_ehci3_utmi_pdata;
		platform_device_register(&tegra_ehci3_device);
	}
}

#ifndef CONFIG_USE_OF
static struct platform_device *vcm30_t124_spi_devices[] __initdata = {
	&tegra11_spi_device1,
	&tegra11_spi_device4,
};

static struct tegra_spi_platform_data vcm30_t124_spi1_pdata = {
	.dma_req_sel		= 15,
	.spi_max_frequency	= 25000000,
	.clock_always_on	= false,
};

static struct tegra_spi_platform_data vcm30_t124_spi4_pdata = {
	.dma_req_sel		= 18,
	.spi_max_frequency	= 25000000,
	.clock_always_on	= false,
};

static void __init vcm30_t124_spi_init(void)
{
	tegra11_spi_device1.dev.platform_data = &vcm30_t124_spi1_pdata;
	tegra11_spi_device4.dev.platform_data = &vcm30_t124_spi4_pdata;
	platform_add_devices(vcm30_t124_spi_devices,
			ARRAY_SIZE(vcm30_t124_spi_devices));
}
#else
static void __init vcm30_t124_spi_init(void)
{
}
#endif

#ifdef CONFIG_USE_OF
struct of_dev_auxdata vcm30_t124_auxdata_lookup[] __initdata = {
	OF_DEV_AUXDATA("nvidia,tegra114-hsuart", TEGRA_UARTA_BASE,
				"serial-tegra.0", NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-hsuart", TEGRA_UARTB_BASE,
				"serial-tegra.1", NULL),
	OF_DEV_AUXDATA("nvidia,tegra114-hsuart", TEGRA_UARTD_BASE,
				"serial-tegra.3", NULL),
	{}
};
#endif

static void __init tegra_vcm30_t124_early_init(void)
{
	tegra_clk_init_from_table(vcm30_t124_clk_init_table);
	tegra_clk_verify_parents();
	tegra_soc_device_init("vcm30_t124");
}

static void __init tegra_vcm30_t124_late_init(void)
{
	struct board_info board_info;
	tegra_get_board_info(&board_info);
	pr_info("board_info: id:sku:fab:major:minor = 0x%04x:0x%04x:0x%02x:0x%02x:0x%02x\n",
		board_info.board_id, board_info.sku,
		board_info.fab, board_info.major_revision,
		board_info.minor_revision);
	platform_device_register(&tegra_pinmux_device);
	vcm30_t124_pinmux_init();
	vcm30_t124_usb_init();
/*	vcm30_t124_xusb_init(); */
	vcm30_t124_nor_init();
	vcm30_t124_i2c_init();
	vcm30_t124_spi_init();
	vcm30_t124_uart_init();
	platform_add_devices(vcm30_t124_devices,
			ARRAY_SIZE(vcm30_t124_devices));
	tegra_io_dpd_init();
	vcm30_t124_sdhci_init();
	vcm30_t124_regulator_init();
	/* vcm30_t124_suspend_init(); */
#if 0
	vcm30_t124_emc_init();
	vcm30_t124_edp_init();
#endif
	isomgr_init();
	/* vcm30_t124_panel_init(); */
	/* vcm30_t124_pmon_init(); */
	vcm30_t124_pcie_init();
	vcm30_t124_sata_init();
#ifdef CONFIG_TEGRA_WDT_RECOVERY
	tegra_wdt_recovery_init();
#endif
	/* FIXME: Required? */
#if 0
	tegra_serial_debug_init(TEGRA_UARTD_BASE, INT_WDT_CPU, NULL, -1, -1);

	vcm30_t124_sensors_init();
	vcm30_t124_soctherm_init();
#endif
	tegra_register_fuse();
}

static void __init vcm30_t124_ramconsole_reserve(unsigned long size)
{
	tegra_ram_console_debug_reserve(SZ_1M);
}

static void __init tegra_vcm30_t124_dt_init(void)
{
	tegra_get_board_info(&board_info);
	tegra_get_display_board_info(&display_board_info);

	tegra_vcm30_t124_early_init();
#ifdef CONFIG_USE_OF
	of_platform_populate(NULL,
		of_default_bus_match_table, vcm30_t124_auxdata_lookup,
		&platform_bus);
#endif

	tegra_vcm30_t124_late_init();
}

static void __init tegra_vcm30_t124_reserve(void)
{
#if defined(CONFIG_NVMAP_CONVERT_CARVEOUT_TO_IOVMM)
	/* 1920*1200*4*2 = 18432000 bytes */
	tegra_reserve(0, SZ_16M + SZ_2M, SZ_16M);
#else
	tegra_reserve(SZ_128M, SZ_16M + SZ_2M, SZ_4M);
#endif
	vcm30_t124_ramconsole_reserve(SZ_1M);
}

static const char * const vcm30_t124_dt_board_compat[] = {
	"nvidia,vcm30_t124",
	NULL
};

DT_MACHINE_START(VCM30_T124, "vcm30_t124")
	.atag_offset	= 0x100,
	.smp		= smp_ops(tegra_smp_ops),
	.map_io		= tegra_map_common_io,
	.reserve	= tegra_vcm30_t124_reserve,
	.init_early	= tegra12x_init_early,
	.init_irq	= tegra_dt_init_irq,
        .init_time      = tegra_init_timer,
	.init_machine	= tegra_vcm30_t124_dt_init,
	.restart	= tegra_assert_system_reset,
	.dt_compat	= vcm30_t124_dt_board_compat,
        .init_late      = tegra_init_late
MACHINE_END
