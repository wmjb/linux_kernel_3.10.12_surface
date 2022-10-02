/*
 * arch/arm/mach-tegra/board-tn8-power.c
 *
 * Copyright (c) 2013 NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/io.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>
#include <linux/irq.h>

#include <linux/gpio.h>
#include <linux/power/bq2419x-charger.h>
#include <linux/power/power_supply_extcon.h>
#include <linux/max17048_battery.h>
#include <linux/tegra-soc.h>
#include <linux/generic_adc_thermal.h>

#include <mach/irqs.h>

#include <asm/mach-types.h>

#include "pm.h"
#include "board.h"
#include "tegra-board-id.h"
#include "board-common.h"
#include "board-ardbeg.h"
#include "board-pmu-defines.h"
#include "devices.h"
#include "iomap.h"
#include "tegra-board-id.h"

#define PMC_CTRL                0x0
#define PMC_CTRL_INTR_LOW       (1 << 17)

/* -40 to 125 degC */
static int tn8_batt_temperature_table[] = {
	259, 266, 272, 279, 286, 293, 301, 308,
	316, 324, 332, 340, 349, 358, 367, 376,
	386, 395, 405, 416, 426, 437, 448, 459,
	471, 483, 495, 508, 520, 533, 547, 561,
	575, 589, 604, 619, 634, 650, 666, 682,
	699, 716, 733, 751, 769, 787, 806, 825,
	845, 865, 885, 905, 926, 947, 969, 990,
	1013, 1035, 1058, 1081, 1104, 1127, 1151, 1175,
	1199, 1224, 1249, 1273, 1298, 1324, 1349, 1374,
	1400, 1426, 1451, 1477, 1503, 1529, 1554, 1580,
	1606, 1631, 1657, 1682, 1707, 1732, 1757, 1782,
	1807, 1831, 1855, 1878, 1902, 1925, 1948, 1970,
	1992, 2014, 2036, 2057, 2077, 2097, 2117, 2136,
	2155, 2174, 2192, 2209, 2227, 2243, 2259, 2275,
	2291, 2305, 2320, 2334, 2347, 2361, 2373, 2386,
	2397, 2409, 2420, 2430, 2441, 2450, 2460, 2469,
	2478, 2486, 2494, 2502, 2509, 2516, 2523, 2529,
	2535, 2541, 2547, 2552, 2557, 2562, 2567, 2571,
	2575, 2579, 2583, 2587, 2590, 2593, 2596, 2599,
	2602, 2605, 2607, 2609, 2611, 2614, 2615, 2617,
	2619, 2621, 2622, 2624, 2625, 2626,
};

/* BQ2419X VBUS regulator */
static struct regulator_consumer_supply bq2419x_vbus_supply[] = {
	REGULATOR_SUPPLY("usb_vbus", "tegra-ehci.0"),
	REGULATOR_SUPPLY("usb_vbus", "tegra-otg"),
};

static struct regulator_consumer_supply bq2419x_batt_supply[] = {
	REGULATOR_SUPPLY("usb_bat_chg", "tegra-udc.0"),
};

static struct bq2419x_vbus_platform_data bq2419x_vbus_pdata = {
	.gpio_otg_iusb = TEGRA_GPIO_PI4,
	.num_consumer_supplies = ARRAY_SIZE(bq2419x_vbus_supply),
	.consumer_supplies = bq2419x_vbus_supply,
};

static struct bq2419x_charger_platform_data bq2419x_charger_pdata = {
	.max_charge_current_mA = 3000,
	.charging_term_current_mA = 100,
	.consumer_supplies = bq2419x_batt_supply,
	.num_consumer_supplies = ARRAY_SIZE(bq2419x_batt_supply),
	.wdt_timeout    = 40,
	.rtc_alarm_time = 3600,
	.chg_restart_time = 1800,
};

struct bq2419x_platform_data tn8_bq2419x_pdata = {
	.vbus_pdata = &bq2419x_vbus_pdata,
};

static struct i2c_board_info __initdata bq2419x_boardinfo[] = {
	{
		I2C_BOARD_INFO("bq2419x", 0x6b),
		.platform_data = &tn8_bq2419x_pdata,
	},
};


static struct gadc_thermal_platform_data gadc_thermal_battery_pdata = {
	.iio_channel_name = "battery-temp-channel",
	.tz_name = "battery-temp",
	.temp_offset = 0,
	.adc_to_temp = NULL,
	.adc_temp_lookup = tn8_batt_temperature_table,
	.lookup_table_size = ARRAY_SIZE(tn8_batt_temperature_table),
	.first_index_temp = 125,
	.last_index_temp = -40,
};

static struct platform_device gadc_thermal_battery = {
	.name   = "generic-adc-thermal",
	.id     = 0,
	.dev    = {
		.platform_data = &gadc_thermal_battery_pdata,
	},
};


static struct power_supply_extcon_plat_data extcon_pdata = {
	.extcon_name = "tegra-udc",
};

static struct platform_device power_supply_extcon_device = {
	.name	= "power-supply-extcon",
	.id	= -1,
	.dev	= {
		.platform_data = &extcon_pdata,
	},
};

int __init tn8_regulator_init(void)
{
	void __iomem *pmc = IO_ADDRESS(TEGRA_PMC_BASE);
	u32 pmc_ctrl;

	/* TPS65913: Normal state of INT request line is LOW.
	 * configure the power management controller to trigger PMU
	 * interrupts when HIGH.
	 */
	pmc_ctrl = readl(pmc + PMC_CTRL);
	writel(pmc_ctrl | PMC_CTRL_INTR_LOW, pmc + PMC_CTRL);

	/* Default PJ0 is connected to charger stat,
	 * HW rework is needed to connect to charger-int.
	 * Do not configure the charger int by default.
	 */
	/* bq2419x_boardinfo[0].irq = gpio_to_irq(TEGRA_GPIO_PJ0); */
	if (get_power_supply_type() == POWER_SUPPLY_TYPE_BATTERY)
		tn8_bq2419x_pdata.bcharger_pdata = &bq2419x_charger_pdata;
	else
		tn8_bq2419x_pdata.bcharger_pdata = NULL;

	i2c_register_board_info(0, bq2419x_boardinfo,
		ARRAY_SIZE(bq2419x_boardinfo));

	platform_device_register(&gadc_thermal_battery);
	platform_device_register(&power_supply_extcon_device);
	return 0;
}

int __init tn8_fixed_regulator_init(void)
{
	return 0;
}
