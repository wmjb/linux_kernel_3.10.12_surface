/*
 * drivers/video/tegra/host/t20/t20.h
 *
 * Tegra Graphics Chip support for T20
 *
 * Copyright (c) 2011-2012, NVIDIA Corporation.
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
#ifndef _NVHOST_T20_H_
#define _NVHOST_T20_H_

#include "chip_support.h"

struct nvhost_master;
struct nvhost_chip_support;
struct output;
struct nvhost_job;
struct nvhost_channel;

#ifdef TEGRA_2X_OR_HIGHER_CONFIG
int nvhost_init_t20_support(struct nvhost_master *,
	struct nvhost_chip_support *);
#else
static inline int nvhost_init_t20_support(struct nvhost_master *host,
					  struct nvhost_chip_support *op)
{
	return -ENODEV;
}
#endif

extern struct nvhost_device_data t20_host1x_info;
extern struct nvhost_device_data t20_gr3d_info;
extern struct nvhost_device_data t20_gr2d_info;
extern struct nvhost_device_data t20_isp_info;
extern struct nvhost_device_data t20_vi_info;
extern struct nvhost_device_data t20_mpe_info;

#endif /* _NVHOST_T20_H_ */
