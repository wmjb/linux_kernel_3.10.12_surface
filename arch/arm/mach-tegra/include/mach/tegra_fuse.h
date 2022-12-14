/*
 * arch/arm/mach-tegra/include/mach/tegra_fuse.h
 *
 * Tegra Public Fuse header file
 *
 * Copyright (c) 2011-2013, NVIDIA Corporation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _MACH_TEGRA_PUBLIC_FUSE_H_
#define _MACH_TEGRA_PUBLIC_FUSE_H_

int tegra_fuse_get_revision(u32 *rev);
int tegra_fuse_get_tsensor_calibration_data(u32 *calib);
int tegra_fuse_get_tsensor_spare_bits(u32 *spare_bits);
#if defined(CONFIG_ARCH_TEGRA_11x_SOC) || defined(CONFIG_ARCH_TEGRA_14x_SOC) \
		|| defined(CONFIG_ARCH_TEGRA_12x_SOC)
int tegra_fuse_get_tsensor_calib(int index, u32 *calib);
int tegra_fuse_calib_base_get_cp(u32 *base_cp, s32 *shifted_cp);
int tegra_fuse_calib_base_get_ft(u32 *base_ft, s32 *shifted_ft);
#endif

#endif /* _MACH_TEGRA_PUBLIC_FUSE_H_*/

