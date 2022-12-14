/*
 * arch/arm/mach-tegra/mcerr-t3.c
 *
 * Tegra 3 SoC-specific mcerr code.
 *
 * Copyright (c) 2010-2013, NVIDIA Corporation. All rights reserved.
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

#include "mcerr.h"

/*** Auto generated by `mcp.pl'. Do not modify! ***/

#define dummy_client   client("dummy", "dummy")

struct mc_client mc_clients[] = {
	client("ptc", "csr_ptcr"),
	client("dc", "cbr_display0a"),
	client("dcb", "cbr_display0ab"),
	client("dc", "cbr_display0b"),
	client("dcb", "cbr_display0bb"),
	client("dc", "cbr_display0c"),
	client("dcb", "cbr_display0cb"),
	client("dc", "cbr_display1b"),
	client("dcb", "cbr_display1bb"),
	client("epp", "cbr_eppup"),
	client("g2", "cbr_g2pr"),
	client("g2", "cbr_g2sr"),
	client("mpe", "cbr_mpeunifbr"),
	client("vi", "cbr_viruv"),
	client("afi", "csr_afir"),
	client("avpc", "csr_avpcarm7r"),
	client("dc", "csr_displayhc"),
	client("dcb", "csr_displayhcb"),
	client("nv", "csr_fdcdrd"),
	client("nv2", "csr_fdcdrd2"),
	client("g2", "csr_g2dr"),
	client("hda", "csr_hdar"),
	client("hc", "csr_host1xdmar"),
	client("hc", "csr_host1xr"),
	client("nv", "csr_idxsrd"),
	client("nv2", "csr_idxsrd2"),
	client("mpe", "csr_mpe_ipred"),
	client("mpe", "csr_mpeamemrd"),
	client("mpe", "csr_mpecsrd"),
	client("ppcs", "csr_ppcsahbdmar"),
	client("ppcs", "csr_ppcsahbslvr"),
	client("sata", "csr_satar"),
	client("nv", "csr_texsrd"),
	client("nv2", "csr_texsrd2"),
	client("vde", "csr_vdebsevr"),
	client("vde", "csr_vdember"),
	client("vde", "csr_vdemcer"),
	client("vde", "csr_vdetper"),
	client("mpcorelp", "csr_mpcorelpr"),
	client("mpcore", "csr_mpcorer"),
	client("epp", "cbw_eppu"),
	client("epp", "cbw_eppv"),
	client("epp", "cbw_eppy"),
	client("mpe", "cbw_mpeunifbw"),
	client("vi", "cbw_viwsb"),
	client("vi", "cbw_viwu"),
	client("vi", "cbw_viwv"),
	client("vi", "cbw_viwy"),
	client("g2", "ccw_g2dw"),
	client("afi", "csw_afiw"),
	client("avpc", "csw_avpcarm7w"),
	client("nv", "csw_fdcdwr"),
	client("nv2", "csw_fdcdwr2"),
	client("hda", "csw_hdaw"),
	client("hc", "csw_host1xw"),
	client("isp", "csw_ispw"),
	client("mpcorelp", "csw_mpcorelpw"),
	client("mpcore", "csw_mpcorew"),
	client("mpe", "csw_mpecswr"),
	client("ppcs", "csw_ppcsahbdmaw"),
	client("ppcs", "csw_ppcsahbslvw"),
	client("sata", "csw_sataw"),
	client("vde", "csw_vdebsevw"),
	client("vde", "csw_vdedbgw"),
	client("vde", "csw_vdembew"),
	client("vde", "csw_vdetpmw"),
};
int mc_client_last = ARRAY_SIZE(mc_clients) - 1;
/*** Done. ***/

/*
 * Defaults work for T30.
 */
void mcerr_chip_specific_setup(struct mcerr_chip_specific *spec)
{
	return;
}
