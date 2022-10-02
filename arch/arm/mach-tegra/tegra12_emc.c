/*
 * arch/arm/mach-tegra/tegra12_emc.c
 *
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
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
 *
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/platform_data/tegra_emc.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/hrtimer.h>
#include <linux/pasr.h>

#include <asm/cputime.h>

#include "clock.h"
#include "board.h"
#include "dvfs.h"
#include "iomap.h"
#include "tegra12_emc.h"

#ifdef CONFIG_TEGRA_EMC_SCALING_ENABLE
static bool emc_enable = true;
#else
static bool emc_enable;
#endif
module_param(emc_enable, bool, 0644);

static int pasr_enable;

u8 tegra_emc_bw_efficiency = 100;

static struct emc_iso_usage tegra12_emc_iso_usage[] = {
	{ BIT(EMC_USER_DC1),                     80 },
	{ BIT(EMC_USER_DC2),                     80 },
	{ BIT(EMC_USER_DC1) | BIT(EMC_USER_DC2),	50 },
	{ BIT(EMC_USER_DC1) | BIT(EMC_USER_VI),  50 },
	{ BIT(EMC_USER_DC2) | BIT(EMC_USER_VI),  50 },
	{ BIT(EMC_USER_DC1) | BIT(EMC_USER_VI2),  50 },
	{ BIT(EMC_USER_DC2) | BIT(EMC_USER_VI2),  50 },
	{ BIT(EMC_USER_DC1) | BIT(EMC_USER_ISP1),  50 },
	{ BIT(EMC_USER_DC2) | BIT(EMC_USER_ISP1),  50 },
	{ BIT(EMC_USER_DC1) | BIT(EMC_USER_ISP2),  50 },
	{ BIT(EMC_USER_DC2) | BIT(EMC_USER_ISP2),  50 },
};

#define PLL_C_DIRECT_FLOOR		333500000
#define EMC_STATUS_UPDATE_TIMEOUT	100
#define TEGRA_EMC_TABLE_MAX_SIZE	16

#define TEGRA_EMC_MODE_REG_17	0x00110000
#define TEGRA_EMC_MRW_DEV_SHIFT	30
#define TEGRA_EMC_MRW_DEV1	2
#define TEGRA_EMC_MRW_DEV2	1

enum {
	DLL_CHANGE_NONE = 0,
	DLL_CHANGE_ON,
	DLL_CHANGE_OFF,
};

#define EMC_CLK_DIV_SHIFT		0
#define EMC_CLK_DIV_MASK		(0xFF << EMC_CLK_DIV_SHIFT)
#define EMC_CLK_SOURCE_SHIFT		29
#define EMC_CLK_SOURCE_MASK		(0x7 << EMC_CLK_SOURCE_SHIFT)
#define EMC_CLK_LOW_JITTER_ENABLE	(0x1 << 31)
#define	EMC_CLK_MC_SAME_FREQ		(0x1 << 16)

#define BURST_REG_LIST \
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RC),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RFC),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RFC_SLR),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RAS),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RP),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_R2W),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_W2R),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_R2P),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_W2P),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RD_RCD),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_WR_RCD),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RRD),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_REXT),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_WEXT),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_WDV),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_WDV_MASK),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_QUSE),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_QUSE_WIDTH),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_IBDLY),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_EINPUT),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_EINPUT_DURATION),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PUTERM_EXTRA),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PUTERM_WIDTH),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PUTERM_ADJ),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CDB_CNTL_1),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CDB_CNTL_2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CDB_CNTL_3),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_QRST),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_QSAFE),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RDV),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RDV_MASK),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_REFRESH),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_BURST_REFRESH_NUM),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PRE_REFRESH_REQ_CNT),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PDEX2WR),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PDEX2RD),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_PCHG2PDEN),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_ACT2PDEN),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_AR2PDEN),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_RW2PDEN),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TXSR),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TXSRDLL),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TCKE),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TCKESR),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TPD),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TFAW),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TRPAB),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TCLKSTABLE),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TCLKSTOP),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TREFBW),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_FBIO_CFG6),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_ODT_WRITE),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_ODT_READ),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_FBIO_CFG5),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CFG_DIG_DLL),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CFG_DIG_DLL_PERIOD),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS0),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS1),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS3),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS4),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS5),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS6),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS7),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS8),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS9),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS10),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS11),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS12),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS13),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS14),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQS15),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE0),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE1),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE2),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE3),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE4),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE5),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE6),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE7),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_ADDR0),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_ADDR1),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_ADDR2),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_ADDR3),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_ADDR4),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_ADDR5),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE8),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE9),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE10),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE11),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE12),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE13),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE14),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_QUSE15),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS0),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS1),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS2),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS3),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS4),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS5),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS6),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS7),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS8),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS9),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS10),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS11),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS12),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS13),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS14),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLI_TRIM_TXDQS15),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ0),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ1),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ3),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ4),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ5),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ6),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DLL_XFORM_DQ7),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2CMDPADCTRL),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2CMDPADCTRL4),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2CMDPADCTRL5),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQSPADCTRL2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQPADCTRL2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQPADCTRL3),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2CLKPADCTRL),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2CLKPADCTRL2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2COMPPADCTRL),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2VTTGENPADCTRL),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2VTTGENPADCTRL2),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2VTTGENPADCTRL3),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQSPADCTRL3),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQSPADCTRL4),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQSPADCTRL5),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_XM2DQSPADCTRL6),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DSR_VTTGEN_DRV),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_TXDSRVTTGEN),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_FBIO_SPARE),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_ZCAL_INTERVAL),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_ZCAL_WAIT_CNT),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_MRS_WAIT_CNT),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_MRS_WAIT_CNT2),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_AUTO_CAL_CONFIG2),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_AUTO_CAL_CONFIG3),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_AUTO_CAL_CONFIG),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CTT),			\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CTT_DURATION),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_CFG_PIPE),		\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_DYN_SELF_REF_CONTROL),	\
	DEFINE_REG(TEGRA_EMC_BASE, EMC_QPOP),			\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_CFG),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_OUTSTANDING_REQ),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_RCD),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_RP),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_RC),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_RAS),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_FAW),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_RRD),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_RAP2PRE),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_WAP2PRE),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_R2R),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_W2W),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_R2W),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_TIMING_W2R),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_DA_TURNS),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_DA_COVERS),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_MISC0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_EMEM_ARB_RING1_THROTTLE),

#define BURST_UP_DOWN_REG_LIST \
	DEFINE_REG(TEGRA_MC_BASE, MC_MLL_MPCORER_PTSA_RATE),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_PTSA_GRANT_DECREMENT),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_XUSB_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_XUSB_1),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_TSEC_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_SDMMCA_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_SDMMCAA_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_SDMMC_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_SDMMCAB_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_PPCS_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_PPCS_1),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_MPCORE_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_MPCORELP_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_HC_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_HC_1),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_AVPC_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_GPU_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_MSENC_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_HDA_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_VIC_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_VI2_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_ISP2_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_ISP2_1),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_ISP2B_0),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_ISP2B_1),	\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_VDE_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_VDE_1),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_VDE_2),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_VDE_3),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_SATA_0),		\
	DEFINE_REG(TEGRA_MC_BASE, MC_LATENCY_ALLOWANCE_AFI_0),

#define DEFINE_REG(base, reg) ((base) ? (IO_ADDRESS((base)) + (reg)) : 0)
static void __iomem *burst_reg_addr[TEGRA12_EMC_MAX_NUM_REGS] = {
	BURST_REG_LIST
};

#ifndef EMULATE_CLOCK_SWITCH
static void __iomem *burst_up_down_reg_addr[TEGRA12_EMC_MAX_NUM_REGS] = {
	BURST_UP_DOWN_REG_LIST
};
#endif
#undef DEFINE_REG

#define DEFINE_REG(base, reg)	reg##_INDEX
enum {
	BURST_REG_LIST
};
#undef DEFINE_REG

struct emc_sel {
	struct clk	*input;
	u32		value;
	unsigned long	input_rate;
};
static struct emc_sel tegra_emc_clk_sel[TEGRA_EMC_TABLE_MAX_SIZE];
static struct tegra12_emc_table start_timing;
static const struct tegra12_emc_table *emc_timing;

static ktime_t clkchange_time;
static int clkchange_delay = 100;

static const u32 *dram_to_soc_bit_map;
static const struct tegra12_emc_table *tegra_emc_table;
static int tegra_emc_table_size;

static u32 dram_dev_num;
static u32 dram_type = -1;

static struct clk *emc;

static struct {
	cputime64_t time_at_clock[TEGRA_EMC_TABLE_MAX_SIZE];
	int last_sel;
	u64 last_update;
	u64 clkchange_count;
	spinlock_t spinlock;
} emc_stats;

static DEFINE_SPINLOCK(emc_access_lock);


static void __iomem *emc_base = IO_ADDRESS(TEGRA_EMC_BASE);
static void __iomem *mc_base = IO_ADDRESS(TEGRA_MC_BASE);
static void __iomem *clk_base = IO_ADDRESS(TEGRA_CLK_RESET_BASE);

static inline void emc_writel(u32 val, unsigned long addr)
{
	writel(val, emc_base + addr);
}

static inline u32 emc_readl(unsigned long addr)
{
	return readl(emc_base + addr);
}
static inline void mc_writel(u32 val, unsigned long addr)
{
	writel(val, mc_base + addr);
}
static inline u32 mc_readl(unsigned long addr)
{
	return readl(mc_base + addr);
}
static inline void ccfifo_writel(u32 val, unsigned long addr)
{
	writel(val, emc_base + EMC_CCFIFO_DATA);
	writel(addr, emc_base + EMC_CCFIFO_ADDR);
}

static int last_round_idx;
static inline int get_start_idx(unsigned long rate)
{
	if (tegra_emc_table[last_round_idx].rate == rate)
		return last_round_idx;
	return 0;
}
static void emc_last_stats_update(int last_sel)
{
	unsigned long flags;
	u64 cur_jiffies = get_jiffies_64();

	spin_lock_irqsave(&emc_stats.spinlock, flags);

	if (emc_stats.last_sel < TEGRA_EMC_TABLE_MAX_SIZE)
		emc_stats.time_at_clock[emc_stats.last_sel] =
			emc_stats.time_at_clock[emc_stats.last_sel] +
			(cur_jiffies - emc_stats.last_update);

	emc_stats.last_update = cur_jiffies;

	if (last_sel < TEGRA_EMC_TABLE_MAX_SIZE) {
		emc_stats.clkchange_count++;
		emc_stats.last_sel = last_sel;
	}
	spin_unlock_irqrestore(&emc_stats.spinlock, flags);
}

static int wait_for_update(u32 status_reg, u32 bit_mask, bool updated_state)
{
	int i;
	for (i = 0; i < EMC_STATUS_UPDATE_TIMEOUT; i++) {
		if (!!(emc_readl(status_reg) & bit_mask) == updated_state)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static inline void emc_timing_update(void)
{
	int err;

	emc_writel(0x1, EMC_TIMING_CONTROL);
	err = wait_for_update(EMC_STATUS,
			      EMC_STATUS_TIMING_UPDATE_STALLED, false);
	if (err) {
		pr_err("%s: timing update error: %d", __func__, err);
		BUG();
	}
}

static inline void auto_cal_disable(void)
{
	int err;

	emc_writel(0, EMC_AUTO_CAL_INTERVAL);
	err = wait_for_update(EMC_AUTO_CAL_STATUS,
			      EMC_AUTO_CAL_STATUS_ACTIVE, false);
	if (err) {
		pr_err("%s: disable auto-cal error: %d", __func__, err);
		BUG();
	}
}

static inline bool dqs_preset(const struct tegra12_emc_table *next_timing,
			      const struct tegra12_emc_table *last_timing)
{
	bool ret = false;
	int data;
#define DQS_SET(reg, bit)						\
	do {						\
		data = emc_readl(EMC_XM2DQSPADCTRL2); \
		if ((next_timing->burst_regs[EMC_##reg##_INDEX] &	\
		     EMC_##reg##_##bit##_ENABLE) &&			\
			(!(data &	\
		       EMC_##reg##_##bit##_ENABLE)))   {		\
				emc_writel(data \
				   | EMC_##reg##_##bit##_ENABLE, EMC_##reg); \
			pr_debug("dqs preset: presetting rx_ft_rec\n");	\
			ret = true;					\
		}							\
	} while (0)
	DQS_SET(XM2DQSPADCTRL2, VREF);
	DQS_SET(XM2DQSPADCTRL2, RX_FT_REC);

	return ret;
}

static inline void overwrite_mrs_wait_cnt(
	const struct tegra12_emc_table *next_timing,
	bool zcal_long)
{
	u32 reg;
	u32 cnt = 512;

	/* For ddr3 when DLL is re-started: overwrite EMC DFS table settings
	   for MRS_WAIT_LONG with maximum of MRS_WAIT_SHORT settings and
	   expected operation length. Reduce the latter by the overlapping
	   zq-calibration, if any */
	if (zcal_long)
		cnt -= dram_dev_num * 256;

	reg = (next_timing->burst_regs[EMC_MRS_WAIT_CNT_INDEX] &
		EMC_MRS_WAIT_CNT_SHORT_WAIT_MASK) >>
		EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT;
	if (cnt < reg)
		cnt = reg;

	reg = (next_timing->burst_regs[EMC_MRS_WAIT_CNT_INDEX] &
		(~EMC_MRS_WAIT_CNT_LONG_WAIT_MASK));
	reg |= (cnt << EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT) &
		EMC_MRS_WAIT_CNT_LONG_WAIT_MASK;

	emc_writel(reg, EMC_MRS_WAIT_CNT);
}

static inline int get_dll_change(const struct tegra12_emc_table *next_timing,
				 const struct tegra12_emc_table *last_timing)
{
	bool next_dll_enabled = !(next_timing->emc_mode_1 & 0x1);
	bool last_dll_enabled = !(last_timing->emc_mode_1 & 0x1);

	if (next_dll_enabled == last_dll_enabled)
		return DLL_CHANGE_NONE;
	else if (next_dll_enabled)
		return DLL_CHANGE_ON;
	else
		return DLL_CHANGE_OFF;
}

static inline void set_dram_mode(const struct tegra12_emc_table *next_timing,
				 const struct tegra12_emc_table *last_timing,
				 int dll_change)
{
	if (dram_type == DRAM_TYPE_DDR3) {
		/* first mode_1, then mode_2, then mode_reset*/
		if (next_timing->emc_mode_1 != last_timing->emc_mode_1)
			ccfifo_writel(next_timing->emc_mode_1, EMC_EMRS);
		if (next_timing->emc_mode_2 != last_timing->emc_mode_2)
			ccfifo_writel(next_timing->emc_mode_2, EMC_EMRS2);

		if ((next_timing->emc_mode_reset !=
		     last_timing->emc_mode_reset) ||
		    (dll_change == DLL_CHANGE_ON)) {
			u32 reg = next_timing->emc_mode_reset &
				(~EMC_MODE_SET_DLL_RESET);
			if (dll_change == DLL_CHANGE_ON) {
				reg |= EMC_MODE_SET_DLL_RESET;
				reg |= EMC_MODE_SET_LONG_CNT;
			}
			ccfifo_writel(reg, EMC_MRS);
		}
	} else {
		/* first mode_2, then mode_1; mode_reset is not applicable */
		if (next_timing->emc_mode_2 != last_timing->emc_mode_2)
			ccfifo_writel(next_timing->emc_mode_2, EMC_MRW2);
		if (next_timing->emc_mode_1 != last_timing->emc_mode_1)
			ccfifo_writel(next_timing->emc_mode_1, EMC_MRW);
		if (next_timing->emc_mode_4 != last_timing->emc_mode_4)
			ccfifo_writel(next_timing->emc_mode_4, EMC_MRW4);
	}
}

static inline void do_clock_change(u32 clk_setting)
{
	int err;

	mc_readl(MC_EMEM_ADR_CFG);	/* completes prev writes */
	emc_readl(EMC_INTSTATUS);

	writel(clk_setting,
		(void __iomem *)((u32)clk_base + emc->reg));
	readl((void __iomem *)((u32)clk_base + emc->reg));
				/* completes prev write */

	err = wait_for_update(EMC_INTSTATUS,
			      EMC_INTSTATUS_CLKCHANGE_COMPLETE, true);
	if (err) {
		pr_err("%s: clock change completion error: %d", __func__, err);
		BUG();
	}
}

static noinline void emc_set_clock(const struct tegra12_emc_table *next_timing,
				   const struct tegra12_emc_table *last_timing,
				   u32 clk_setting)
{
#ifndef EMULATE_CLOCK_SWITCH
	int i, dll_change, pre_wait, ctt_term_changed;
	bool dyn_sref_enabled, zcal_long;

	u32 emc_cfg_reg = emc_readl(EMC_CFG);
	u32 emc_cfg_2_reg = emc_readl(EMC_CFG_2);

	dyn_sref_enabled = emc_cfg_reg & EMC_CFG_DYN_SREF_ENABLE;
	dll_change = get_dll_change(next_timing, last_timing);
	zcal_long = (next_timing->burst_regs[EMC_ZCAL_INTERVAL_INDEX] != 0) &&
		(last_timing->burst_regs[EMC_ZCAL_INTERVAL_INDEX] == 0);

	/* 1. clear clkchange_complete interrupts */
	emc_writel(EMC_INTSTATUS_CLKCHANGE_COMPLETE, EMC_INTSTATUS);


	/* 1.5 On t124,  prelock the DLL - assuming the DLL is enabled. */
	/* TODO: implement. */

	/* 2. disable dynamic self-refresh and preset dqs vref, then wait for
	   possible self-refresh entry/exit and/or dqs vref settled - waiting
	   before the clock change decreases worst case change stall time */
	pre_wait = 0;
	if (dyn_sref_enabled) {
		emc_cfg_reg &= ~EMC_CFG_DYN_SREF_ENABLE;
		emc_writel(emc_cfg_reg, EMC_CFG);
		pre_wait = 5;		/* 5us+ for self-refresh entry/exit */
	}

	/* 2.5 check dq/dqs vref delay */
	if (dqs_preset(next_timing, last_timing)) {
		if (pre_wait < 30)
			pre_wait = 30;	/* 3us+ for dqs vref settled */
	}

	/* 2.6 Program CTT_TERM Control if it changed since last time*/
	/* PLACE HOLDER FOR NOW , CODE TO BE ADDED
	Bug-1258083, software hack for updating EMC_CCT_TERM_CTRL
	/term-slope,offset values instantly*/
	ctt_term_changed = (last_timing->emc_ctt_term_ctrl
				!= next_timing->emc_ctt_term_ctrl);
	if (last_timing->emc_ctt_term_ctrl !=
			next_timing->emc_ctt_term_ctrl) {
			auto_cal_disable();
			emc_writel(next_timing->emc_ctt_term_ctrl,
				EMC_CTT_TERM_CTRL);
	}

	if (pre_wait || ctt_term_changed) {
		emc_timing_update();
		udelay(pre_wait);
	}

	/* 3. disable auto-cal if vref mode is switching - removed */

	/* 4. program burst shadow registers */
	for (i = 0; i < next_timing->burst_regs_num; i++) {
		if (!burst_reg_addr[i])
			continue;
		__raw_writel(next_timing->burst_regs[i], burst_reg_addr[i]);
	}

	emc_cfg_reg &= ~EMC_CFG_UPDATE_MASK;
	emc_cfg_reg |= next_timing->emc_cfg & EMC_CFG_UPDATE_MASK;
	emc_writel(emc_cfg_reg, EMC_CFG);
	wmb();
	barrier();

	/* 4.1 On ddr3 when DLL is re-started predict MRS long wait count and
	   overwrite DFS table setting  */
	if ((dram_type == DRAM_TYPE_DDR3) && (dll_change == DLL_CHANGE_ON))
		overwrite_mrs_wait_cnt(next_timing, zcal_long);

	/* 5.2 disable auto-refresh to save time after clock change */
	emc_writel(EMC_REFCTRL_DISABLE_ALL(dram_dev_num), EMC_REFCTRL);

	/* 5.3 post cfg_2 write and dis ob clock gate */
	emc_cfg_2_reg = next_timing->emc_cfg_2;

	if (emc_cfg_2_reg & EMC_CFG_2_DIS_STP_OB_CLK_DURING_NON_WR)
		emc_cfg_2_reg &= ~EMC_CFG_2_DIS_STP_OB_CLK_DURING_NON_WR;
	ccfifo_writel(emc_cfg_2_reg, EMC_CFG_2);

	/* 5.4 program sel_dpd */
	ccfifo_writel(next_timing->emc_sel_dpd_ctrl, EMC_SEL_DPD_CTRL);

	/* 6. turn Off dll and enter self-refresh on DDR3  */
	if (dram_type == DRAM_TYPE_DDR3) {
		if (dll_change == DLL_CHANGE_OFF)
			ccfifo_writel(next_timing->emc_mode_1, EMC_EMRS);
		ccfifo_writel(DRAM_BROADCAST(dram_dev_num) |
			      EMC_SELF_REF_CMD_ENABLED, EMC_SELF_REF);
	}

	/* 7. flow control marker 2 */
	ccfifo_writel(1, EMC_STALL_THEN_EXE_AFTER_CLKCHANGE);

	/* 8. exit self-refresh on DDR3 */
	if (dram_type == DRAM_TYPE_DDR3)
		ccfifo_writel(DRAM_BROADCAST(dram_dev_num), EMC_SELF_REF);

	/* 9. set dram mode registers */
	set_dram_mode(next_timing, last_timing, dll_change);

	/* 10. issue zcal command if turning zcal On */
	if (zcal_long) {
		ccfifo_writel(EMC_ZQ_CAL_LONG_CMD_DEV0, EMC_ZQ_CAL);
		if (dram_dev_num > 1)
			ccfifo_writel(EMC_ZQ_CAL_LONG_CMD_DEV1, EMC_ZQ_CAL);
	}

	/* 10.1 dummy write to RO register to remove stall after change */
	ccfifo_writel(0, EMC_CCFIFO_STATUS);


	/* 11.1 DIS_STP_OB_CLK_DURING_NON_WR ->0 */
	if (next_timing->emc_cfg_2 & EMC_CFG_2_DIS_STP_OB_CLK_DURING_NON_WR) {
		emc_cfg_2_reg = next_timing->emc_cfg_2;
		ccfifo_writel(emc_cfg_2_reg, EMC_CFG_2);
	}

	/* 11.5 program burst_up_down registers if emc rate is going down */
	if (next_timing->rate < last_timing->rate) {
		for (i = 0; i < next_timing->burst_up_down_regs_num; i++)
			__raw_writel(next_timing->burst_up_down_regs[i],
				burst_up_down_reg_addr[i]);
		wmb();
	}

	/* 12-14. read any MC register to ensure the programming is done
	   change EMC clock source register wait for clk change completion */
	do_clock_change(clk_setting);

	/* 14.1 re-enable auto-refresh */
	emc_writel(EMC_REFCTRL_ENABLE_ALL(dram_dev_num), EMC_REFCTRL);

	/* 14.2 program burst_up_down registers if emc rate is going up */
	if (next_timing->rate > last_timing->rate) {
		for (i = 0; i < next_timing->burst_up_down_regs_num; i++)
			__raw_writel(next_timing->burst_up_down_regs[i],
				burst_up_down_reg_addr[i]);
		wmb();
	}

	/* 15. restore auto-cal */
	if (last_timing->emc_ctt_term_ctrl != next_timing->emc_ctt_term_ctrl)
		emc_writel(next_timing->emc_acal_interval,
			EMC_AUTO_CAL_INTERVAL);

	/* 16. restore dynamic self-refresh */
	if (next_timing->emc_cfg & EMC_CFG_DYN_SREF_ENABLE) {
		emc_cfg_reg |= EMC_CFG_DYN_SREF_ENABLE;
		emc_writel(emc_cfg_reg, EMC_CFG);
	}

	/* 17. set zcal wait count */
	emc_writel(next_timing->emc_zcal_cnt_long, EMC_ZCAL_WAIT_CNT);

	/* 18. update restored timing */
	udelay(2);
	emc_timing_update();
#else
	/* FIXME: implement */
	pr_info("tegra12_emc: Configuring EMC rate %lu (setting: 0x%x)\n",
		next_timing->rate, clk_setting);
#endif
}

static inline void emc_get_timing(struct tegra12_emc_table *timing)
{
	int i;

	/* Burst updates depends on previous state; burst_up_down are
	 * stateless. */
	for (i = 0; i < timing->burst_regs_num; i++) {
		if (burst_reg_addr[i])
			timing->burst_regs[i] = __raw_readl(burst_reg_addr[i]);
		else
			timing->burst_regs[i] = 0;
	}
	timing->emc_acal_interval = 0;
	timing->emc_zcal_cnt_long = 0;
	timing->emc_mode_reset = 0;
	timing->emc_mode_1 = 0;
	timing->emc_mode_2 = 0;
	timing->emc_mode_4 = 0;
	timing->emc_cfg = emc_readl(EMC_CFG);
	timing->rate = clk_get_rate_locked(emc) / 1000;
}

/* The EMC registers have shadow registers. When the EMC clock is updated
 * in the clock controller, the shadow registers are copied to the active
 * registers, allowing glitchless memory bus frequency changes.
 * This function updates the shadow registers for a new clock frequency,
 * and relies on the clock lock on the emc clock to avoid races between
 * multiple frequency changes. In addition access lock prevents concurrent
 * access to EMC registers from reading MRR registers */
int tegra_emc_set_rate(unsigned long rate)
{
	int i;
	u32 clk_setting;
	const struct tegra12_emc_table *last_timing;
	unsigned long flags;
	s64 last_change_delay;

	if (!tegra_emc_table)
		return -EINVAL;

	/* Table entries specify rate in kHz */
	rate = rate / 1000;

	i = get_start_idx(rate);
	for (; i < tegra_emc_table_size; i++) {
		if (tegra_emc_clk_sel[i].input == NULL)
			continue;	/* invalid entry */

		if (tegra_emc_table[i].rate == rate)
			break;
	}

	if (i >= tegra_emc_table_size)
		return -EINVAL;

	if (!emc_timing) {
		/* can not assume that boot timing matches dfs table even
		   if boot frequency matches one of the table nodes */
		emc_get_timing(&start_timing);
		last_timing = &start_timing;
	} else
		last_timing = emc_timing;

	clk_setting = tegra_emc_clk_sel[i].value;

	last_change_delay = ktime_us_delta(ktime_get(), clkchange_time);
	if ((last_change_delay >= 0) && (last_change_delay < clkchange_delay))
		udelay(clkchange_delay - (int)last_change_delay);

	spin_lock_irqsave(&emc_access_lock, flags);
	emc_set_clock(&tegra_emc_table[i], last_timing, clk_setting);
	clkchange_time = ktime_get();
	emc_timing = &tegra_emc_table[i];
	spin_unlock_irqrestore(&emc_access_lock, flags);

	emc_last_stats_update(i);

	pr_debug("%s: rate %lu setting 0x%x\n", __func__, rate, clk_setting);

	return 0;
}

long tegra_emc_round_rate_updown(unsigned long rate, bool up)
{
	int i;
	unsigned long table_rate;

	if (!tegra_emc_table)
		return clk_get_rate_locked(emc); /* no table - no rate change */

	if (!emc_enable)
		return -EINVAL;

	pr_debug("%s: %lu\n", __func__, rate);

	/* Table entries specify rate in kHz */
	rate = rate / 1000;

	i = get_start_idx(rate);
	for (; i < tegra_emc_table_size; i++) {
		if (tegra_emc_clk_sel[i].input == NULL)
			continue;	/* invalid entry */

		table_rate = tegra_emc_table[i].rate;
		if (table_rate >= rate) {
			if (!up && i && (table_rate > rate)) {
				i--;
				table_rate = tegra_emc_table[i].rate;
			}
			pr_debug("%s: using %lu\n", __func__, table_rate);
			last_round_idx = i;
			return table_rate * 1000;
		}
	}

	return -EINVAL;
}

struct clk *tegra_emc_predict_parent(unsigned long rate, u32 *div_value)
{
	int i;

	if (!tegra_emc_table) {
		if (rate == clk_get_rate_locked(emc)) {
			*div_value = emc->div - 2;
			return emc->parent;
		}
		return NULL;
	}

	pr_debug("%s: %lu\n", __func__, rate);

	/* Table entries specify rate in kHz */
	rate = rate / 1000;

	i = get_start_idx(rate);
	for (; i < tegra_emc_table_size; i++) {
		if (tegra_emc_table[i].rate == rate) {
			struct clk *p = tegra_emc_clk_sel[i].input;

			if (p && (tegra_emc_clk_sel[i].input_rate ==
				  clk_get_rate(p))) {
				*div_value = (tegra_emc_clk_sel[i].value &
					EMC_CLK_DIV_MASK) >> EMC_CLK_DIV_SHIFT;
				return p;
			}
		}
	}
	return NULL;
}

bool tegra_emc_is_parent_ready(unsigned long rate, struct clk **parent,
		unsigned long *parent_rate, unsigned long *backup_rate)
{

	int i;
	struct clk *p = NULL;
	unsigned long p_rate = 0;

	if (!tegra_emc_table || !emc_enable)
		return true;

	pr_debug("%s: %lu\n", __func__, rate);

	/* Table entries specify rate in kHz */
	rate = rate / 1000;

	i = get_start_idx(rate);
	for (; i < tegra_emc_table_size; i++) {
		if (tegra_emc_table[i].rate == rate) {
			p = tegra_emc_clk_sel[i].input;
			if (!p)
				continue;	/* invalid entry */

			p_rate = tegra_emc_clk_sel[i].input_rate;
			if (p_rate == clk_get_rate(p))
				return true;
			break;
		}
	}

	/* Table match not found - "non existing parent" is ready */
	if (!p)
		return true;

#ifdef CONFIG_TEGRA_PLLM_SCALED
	/*
	 * Table match found, but parent is not ready - check if backup entry
	 * was found during initialization, and return the respective backup
	 * rate
	 */
	if (emc->shared_bus_backup.input &&
	    (emc->shared_bus_backup.input != p)) {
		*parent = p;
		*parent_rate = p_rate;
		*backup_rate = emc->shared_bus_backup.bus_rate;
		return false;
	}
#else
	/*
	 * Table match found, but parent is not ready - continue search
	 * for backup rate: min rate above requested that has different
	 * parent source (since only pll_c is scaled and may not be ready,
	 * any other parent can provide backup)
	 */
	*parent = p;
	*parent_rate = p_rate;

	for (i++; i < tegra_emc_table_size; i++) {
		p = tegra_emc_clk_sel[i].input;
		if (!p)
			continue;	/* invalid entry */

		if (p != (*parent)) {
			*backup_rate = tegra_emc_table[i].rate * 1000;
			return false;
		}
	}
#endif
	/* Parent is not ready, and no backup found */
	*backup_rate = -EINVAL;
	return false;
}

static inline const struct clk_mux_sel *get_emc_input(u32 val)
{
	const struct clk_mux_sel *sel;

	for (sel = emc->inputs; sel->input != NULL; sel++) {
		if (sel->value == val)
			break;
	}
	return sel;
}

static int find_matching_input(const struct tegra12_emc_table *table,
	struct clk *pll_c, struct clk *pll_m, struct emc_sel *emc_clk_sel)
{
	u32 div_value = (table->src_sel_reg & EMC_CLK_DIV_MASK) >>
		EMC_CLK_DIV_SHIFT;
	u32 src_value = (table->src_sel_reg & EMC_CLK_SOURCE_MASK) >>
		EMC_CLK_SOURCE_SHIFT;
	unsigned long input_rate = 0;
	unsigned long table_rate = table->rate * 1000; /* table rate in kHz */
	const struct clk_mux_sel *sel = get_emc_input(src_value);

#ifdef CONFIG_TEGRA_PLLM_SCALED
	struct clk *scalable_pll = pll_m;
#else
	struct clk *scalable_pll = pll_c;
#endif
	pr_info_once("tegra: %s is selected as scalable EMC clock source\n",
		     scalable_pll->name);

	if (div_value & 0x1) {
		pr_warn("tegra: invalid odd divider for EMC rate %lu\n",
			table_rate);
		return -EINVAL;
	}
	if (!sel->input) {
		pr_warn("tegra: no matching input found for EMC rate %lu\n",
			table_rate);
		return -EINVAL;
	}
	if (div_value && (table->src_sel_reg & EMC_CLK_LOW_JITTER_ENABLE)) {
		pr_warn("tegra: invalid LJ path for EMC rate %lu\n",
			table_rate);
		return -EINVAL;
	}
	if (!(table->src_sel_reg & EMC_CLK_MC_SAME_FREQ) !=
	    !(MC_EMEM_ARB_MISC0_EMC_SAME_FREQ &
	      table->burst_regs[MC_EMEM_ARB_MISC0_INDEX])) {
		pr_warn("tegra: ambiguous EMC to MC ratio for EMC rate %lu\n",
			table_rate);
		return -EINVAL;
	}

#ifndef CONFIG_TEGRA_DUAL_CBUS
	if (sel->input == pll_c) {
		pr_warn("tegra: %s is cbus source: no EMC rate %lu support\n",
			sel->input->name, table_rate);
		return -EINVAL;
	}
#endif

	if (sel->input == scalable_pll) {
		input_rate = table_rate * (1 + div_value / 2);
	} else {
		/* all other sources are fixed, must exactly match the rate */
		input_rate = clk_get_rate(sel->input);
		if (input_rate != (table_rate * (1 + div_value / 2))) {
			pr_warn("tegra: EMC rate %lu does not match %s rate %lu\n",
				table_rate, sel->input->name, input_rate);
			return -EINVAL;
		}
	}

#ifdef CONFIG_TEGRA_PLLM_SCALED
		if (sel->input == pll_c) {
			/* maybe overwritten in a loop - end up at max rate
			   from pll_c */
			emc->shared_bus_backup.input = pll_c;
			emc->shared_bus_backup.bus_rate = table_rate;
		}
#endif
	/* Get ready emc clock selection settings for this table rate */
	emc_clk_sel->input = sel->input;
	emc_clk_sel->input_rate = input_rate;
	emc_clk_sel->value = table->src_sel_reg;

	return 0;
}


static int emc_core_millivolts[MAX_DVFS_FREQS];

static void adjust_emc_dvfs_table(const struct tegra12_emc_table *table,
				  int table_size)
{
	int i, j, mv;
	unsigned long rate;

	BUG_ON(table_size > MAX_DVFS_FREQS);

	for (i = 0, j = 0; j < table_size; j++) {
		if (tegra_emc_clk_sel[j].input == NULL)
			continue;	/* invalid entry */

		rate = table[j].rate * 1000;
		mv = table[j].emc_min_mv;

		if ((i == 0) || (mv > emc_core_millivolts[i-1])) {
			/* advance: voltage has increased */
			emc->dvfs->freqs[i] = rate;
			emc_core_millivolts[i] = mv;
			i++;
		} else {
			/* squash: voltage has not increased */
			emc->dvfs->freqs[i-1] = rate;
		}
	}

	emc->dvfs->millivolts = emc_core_millivolts;
	emc->dvfs->num_freqs = i;
}

#ifdef CONFIG_TEGRA_PLLM_SCALED
/* When pll_m is scaled, pll_c must provide backup rate;
   if not - remove rates that require pll_m scaling */
static int purge_emc_table(unsigned long max_rate)
{
	int i;
	int ret = 0;

	if (emc->shared_bus_backup.input)
		return ret;

	pr_warn("tegra: selected pll_m scaling option but no backup source:\n");
	pr_warn("       removed not supported entries from the table:\n");

	/* made all entries with non matching rate invalid */
	for (i = 0; i < tegra_emc_table_size; i++) {
		struct emc_sel *sel = &tegra_emc_clk_sel[i];
		if (sel->input) {
			if (clk_get_rate(sel->input) != sel->input_rate) {
				pr_warn("       EMC rate %lu\n",
					tegra_emc_table[i].rate * 1000);
				sel->input = NULL;
				sel->input_rate = 0;
				sel->value = 0;
				if (max_rate == tegra_emc_table[i].rate)
					ret = -EINVAL;
			}
		}
	}
	return ret;
}
#else
/* When pll_m is fixed @ max EMC rate, it always provides backup for pll_c */
#define purge_emc_table(max_rate) (0)
#endif

static int init_emc_table(const struct tegra12_emc_table *table, int table_size)
{
	int i, mv;
	u32 reg;
	bool max_entry = false;
	bool emc_max_dvfs_sel = get_emc_max_dvfs();
	unsigned long boot_rate, max_rate;
	struct clk *pll_c = tegra_get_clock_by_name("pll_c");
	struct clk *pll_m = tegra_get_clock_by_name("pll_m");

	emc_stats.clkchange_count = 0;
	spin_lock_init(&emc_stats.spinlock);
	emc_stats.last_update = get_jiffies_64();
	emc_stats.last_sel = TEGRA_EMC_TABLE_MAX_SIZE;

	if (dram_type != DRAM_TYPE_DDR3) {
		pr_err("tegra: not supported DRAM type %u\n", dram_type);
		return -ENODATA;
	}

	if (!table || !table_size) {
		pr_err("tegra: EMC DFS table is empty\n");
		return -ENODATA;
	}

	boot_rate = clk_get_rate(emc) / 1000;
	max_rate = boot_rate;

	tegra_emc_table_size = min(table_size, TEGRA_EMC_TABLE_MAX_SIZE);
	switch (table[0].rev) {
	case 0x14:
	case 0x15:
		start_timing.burst_regs_num = table[0].burst_regs_num;
		break;
	default:
		pr_err("tegra: invalid EMC DFS table: unknown rev 0x%x\n",
			table[0].rev);
		return -ENODATA;
	}

	/* Match EMC source/divider settings with table entries */
	for (i = 0; i < tegra_emc_table_size; i++) {
		unsigned long table_rate = table[i].rate;

		/* Stop: "no-rate" entry, or entry violating ascending order */
		if (!table_rate || (i && ((table_rate <= table[i-1].rate) ||
			(table[i].emc_min_mv < table[i-1].emc_min_mv)))) {
			pr_warn("tegra: EMC rate entry %lu is not ascending\n",
				table_rate);
			break;
		}

		BUG_ON(table[i].rev != table[0].rev);

		if (find_matching_input(&table[i], pll_c, pll_m,
					&tegra_emc_clk_sel[i]))
			continue;

		if (table_rate == boot_rate)
			emc_stats.last_sel = i;

		if (emc_max_dvfs_sel) {
			/* EMC max rate = max table entry above boot rate */
			if (table_rate >= max_rate) {
				max_rate = table_rate;
				max_entry = true;
			}
		} else if (table_rate == max_rate) {
			/* EMC max rate = boot rate */
			max_entry = true;
			break;
		}
	}

	/* Validate EMC rate and voltage limits */
	if (!max_entry) {
		pr_err("tegra: invalid EMC DFS table: entry for max rate"
		       " %lu kHz is not found\n", max_rate);
		return -ENODATA;
	}

	tegra_emc_table = table;

	/*
	 * Purge rates that cannot be reached because table does not specify
	 * proper backup source. If maximum rate was purged, fall back on boot
	 * rate as maximum limit. In any case propagate new maximum limit
	 * down stream to shared users, and check it against nominal voltage.
	 */
	if (purge_emc_table(max_rate))
		max_rate = boot_rate;
	tegra_init_max_rate(emc, max_rate * 1000);

	if (emc->dvfs) {
		adjust_emc_dvfs_table(tegra_emc_table, tegra_emc_table_size);
		mv = tegra_dvfs_predict_millivolts(emc, max_rate * 1000);
		if ((mv <= 0) || (mv > emc->dvfs->max_millivolts)) {
			tegra_emc_table = NULL;
			pr_err("tegra: invalid EMC DFS table: maximum rate %lu"
			       " kHz does not match nominal voltage %d\n",
			       max_rate, emc->dvfs->max_millivolts);
			return -ENODATA;
		}
	}

	pr_info("tegra: validated EMC DFS table\n");

	/* Configure clock change mode according to dram type */
	reg = emc_readl(EMC_CFG_2) & (~EMC_CFG_2_MODE_MASK);
	reg |= ((dram_type == DRAM_TYPE_LPDDR2) ? EMC_CFG_2_PD_MODE :
		EMC_CFG_2_SREF_MODE) << EMC_CFG_2_MODE_SHIFT;
	emc_writel(reg, EMC_CFG_2);
	return 0;
}

#ifdef CONFIG_PASR
static bool tegra12_is_lpddr3(void)
{
	return (dram_type == DRAM_TYPE_LPDDR2);
}

static void tegra12_pasr_apply_mask(u16 *mem_reg, void *cookie)
{
	u32 val = 0;
	int device = (int)cookie;

	val = TEGRA_EMC_MODE_REG_17 | *mem_reg;
	val |= device << TEGRA_EMC_MRW_DEV_SHIFT;

	emc_writel(val, EMC_MRW);

	pr_debug("%s: cookie = %d mem_reg = 0x%04x val = 0x%08x\n", __func__,
			(int)cookie, *mem_reg, val);
}

static int tegra12_pasr_enable(const char *arg, const struct kernel_param *kp)
{
	unsigned int old_pasr_enable;
	void *cookie;
	u16 mem_reg;

	if (!tegra12_is_lpddr3())
		return -ENOSYS;

	old_pasr_enable = pasr_enable;
	param_set_int(arg, kp);

	if (old_pasr_enable == pasr_enable)
		return 0;

	/* Cookie represents the device number to write to MRW register.
	 * 0x2 to for only dev0, 0x1 for dev1.
	 */
	if (pasr_enable == 0) {
		mem_reg = 0;

		cookie = (void *)(int)TEGRA_EMC_MRW_DEV1;
		if (!pasr_register_mask_function(TEGRA_DRAM_BASE,
							NULL, cookie))
			tegra12_pasr_apply_mask(&mem_reg, cookie);

		cookie = (void *)(int)TEGRA_EMC_MRW_DEV2;
		if (!pasr_register_mask_function(TEGRA_DRAM_BASE + SZ_1G,
							NULL, cookie))
			tegra12_pasr_apply_mask(&mem_reg, cookie);
	} else {
		cookie = (void *)(int)TEGRA_EMC_MRW_DEV1;
		pasr_register_mask_function(TEGRA_DRAM_BASE,
					&tegra12_pasr_apply_mask, cookie);

		cookie = (void *)(int)TEGRA_EMC_MRW_DEV2;
		pasr_register_mask_function(TEGRA_DRAM_BASE + SZ_1G,
					&tegra12_pasr_apply_mask, cookie);
	}

	return 0;
}

static struct kernel_param_ops tegra12_pasr_enable_ops = {
	.set = tegra12_pasr_enable,
	.get = param_get_int,
};
module_param_cb(pasr_enable, &tegra12_pasr_enable_ops, &pasr_enable, 0644);
#endif

static int tegra12_emc_probe(struct platform_device *pdev)
{
	struct tegra12_emc_pdata *pdata;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing register base\n");
		return -ENOMEM;
	}

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "missing platform data\n");
		return -ENODATA;
	}

	return init_emc_table(pdata->tables, pdata->num_tables);
}

static struct platform_driver tegra12_emc_driver = {
	.driver         = {
		.name   = "tegra-emc",
		.owner  = THIS_MODULE,
	},
	.probe          = tegra12_emc_probe,
};

int __init tegra12_emc_init(void)
{
	int ret = platform_driver_register(&tegra12_emc_driver);

	if (!ret) {
		tegra_emc_iso_usage_table_init(tegra12_emc_iso_usage,
				ARRAY_SIZE(tegra12_emc_iso_usage));
		if (emc_enable) {
			unsigned long rate = tegra_emc_round_rate_updown(
				emc->boot_rate, false);
			if (!IS_ERR_VALUE(rate))
				tegra_clk_preset_emc_monitor(rate);
		}
	}
	return ret;
}

void tegra_emc_timing_invalidate(void)
{
	emc_timing = NULL;
}

void tegra_emc_dram_type_init(struct clk *c)
{
	emc = c;

	dram_type = (emc_readl(EMC_FBIO_CFG5) &
		     EMC_CFG5_TYPE_MASK) >> EMC_CFG5_TYPE_SHIFT;

	dram_dev_num = (mc_readl(MC_EMEM_ADR_CFG) & 0x1) + 1; /* 2 dev max */
}

int tegra_emc_get_dram_type(void)
{
	return dram_type;
}

static u32 soc_to_dram_bit_swap(u32 soc_val, u32 dram_mask, u32 dram_shift)
{
	int bit;
	u32 dram_val = 0;

	/* tegra clocks definitions use shifted mask always */
	if (!dram_to_soc_bit_map)
		return soc_val & dram_mask;

	for (bit = dram_shift; bit < 32; bit++) {
		u32 dram_bit_mask = 0x1 << bit;
		u32 soc_bit_mask = dram_to_soc_bit_map[bit];

		if (!(dram_bit_mask & dram_mask))
			break;

		if (soc_bit_mask & soc_val)
			dram_val |= dram_bit_mask;
	}

	return dram_val;
}

static int emc_read_mrr(int dev, int addr)
{
	int ret;
	u32 val, emc_cfg;

	if (dram_type != DRAM_TYPE_LPDDR2)
		return -ENODEV;

	ret = wait_for_update(EMC_STATUS, EMC_STATUS_MRR_DIVLD, false);
	if (ret)
		return ret;

	emc_cfg = emc_readl(EMC_CFG);
	if (emc_cfg & EMC_CFG_DRAM_ACPD) {
		emc_writel(emc_cfg & ~EMC_CFG_DRAM_ACPD, EMC_CFG);
		emc_timing_update();
	}

	val = dev ? DRAM_DEV_SEL_1 : DRAM_DEV_SEL_0;
	val |= (addr << EMC_MRR_MA_SHIFT) & EMC_MRR_MA_MASK;
	emc_writel(val, EMC_MRR);

	ret = wait_for_update(EMC_STATUS, EMC_STATUS_MRR_DIVLD, true);
	if (emc_cfg & EMC_CFG_DRAM_ACPD) {
		emc_writel(emc_cfg, EMC_CFG);
		emc_timing_update();
	}
	if (ret)
		return ret;

	val = emc_readl(EMC_MRR) & EMC_MRR_DATA_MASK;
	return val;
}

int tegra_emc_get_dram_temperature(void)
{
	int mr4;
	unsigned long flags;

	spin_lock_irqsave(&emc_access_lock, flags);

	mr4 = emc_read_mrr(0, 4);
	if (IS_ERR_VALUE(mr4)) {
		spin_unlock_irqrestore(&emc_access_lock, flags);
		return mr4;
	}
	spin_unlock_irqrestore(&emc_access_lock, flags);

	mr4 = soc_to_dram_bit_swap(
		mr4, LPDDR2_MR4_TEMP_MASK, LPDDR2_MR4_TEMP_SHIFT);
	return mr4;
}

#ifdef CONFIG_DEBUG_FS

static struct dentry *emc_debugfs_root;

static int emc_stats_show(struct seq_file *s, void *data)
{
	int i;

	emc_last_stats_update(TEGRA_EMC_TABLE_MAX_SIZE);

	seq_printf(s, "%-10s %-10s\n", "rate kHz", "time");
	for (i = 0; i < tegra_emc_table_size; i++) {
		if (tegra_emc_clk_sel[i].input == NULL)
			continue;	/* invalid entry */

		seq_printf(s, "%-10lu %-10llu\n", tegra_emc_table[i].rate,
			cputime64_to_clock_t(emc_stats.time_at_clock[i]));
	}
	seq_printf(s, "%-15s %llu\n", "transitions:",
		   emc_stats.clkchange_count);
	seq_printf(s, "%-15s %llu\n", "time-stamp:",
		   cputime64_to_clock_t(emc_stats.last_update));

	return 0;
}

static int emc_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, emc_stats_show, inode->i_private);
}

static const struct file_operations emc_stats_fops = {
	.open		= emc_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int emc_table_info_show(struct seq_file *s, void *data)
{
	int i;
	for (i = 0; i < tegra_emc_table_size; i++) {
		if (tegra_emc_clk_sel[i].input == NULL)
			continue;
		seq_printf(s, "Table info:\n   Rev: 0x%02x\n"
		"   Table ID: %s\n", tegra_emc_table[i].rev,
		tegra_emc_table[i].table_id);
		seq_printf(s, "    %lu\n", tegra_emc_table[i].rate);
	}

	return 0;
}

static int emc_table_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, emc_table_info_show, inode->i_private);
}

static const struct file_operations emc_table_info_fops = {
	.open		= emc_table_info_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int dram_temperature_get(void *data, u64 *val)
{
	*val = tegra_emc_get_dram_temperature();
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(dram_temperature_fops, dram_temperature_get,
			NULL, "%lld\n");

static int efficiency_get(void *data, u64 *val)
{
	*val = tegra_emc_bw_efficiency;
	return 0;
}
static int efficiency_set(void *data, u64 val)
{
	tegra_emc_bw_efficiency = (val > 100) ? 100 : val;
	if (emc)
		tegra_clk_shared_bus_update(emc);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(efficiency_fops, efficiency_get,
			efficiency_set, "%llu\n");

static int __init tegra_emc_debug_init(void)
{
	if (!tegra_emc_table)
		return 0;

	emc_debugfs_root = debugfs_create_dir("tegra_emc", NULL);
	if (!emc_debugfs_root)
		return -ENOMEM;

	if (!debugfs_create_file(
		"stats", S_IRUGO, emc_debugfs_root, NULL, &emc_stats_fops))
		goto err_out;

	if (!debugfs_create_u32("clkchange_delay", S_IRUGO | S_IWUSR,
		emc_debugfs_root, (u32 *)&clkchange_delay))
		goto err_out;

	if (!debugfs_create_file("dram_temperature", S_IRUGO, emc_debugfs_root,
				 NULL, &dram_temperature_fops))
		goto err_out;

	if (!debugfs_create_file("efficiency", S_IRUGO | S_IWUSR,
				 emc_debugfs_root, NULL, &efficiency_fops))
		goto err_out;


	if (tegra_emc_iso_usage_debugfs_init(emc_debugfs_root))
		goto err_out;

	if (!debugfs_create_file("table_info", S_IRUGO,
				 emc_debugfs_root, NULL, &emc_table_info_fops))
		goto err_out;

	return 0;

err_out:
	debugfs_remove_recursive(emc_debugfs_root);
	return -ENOMEM;
}

late_initcall(tegra_emc_debug_init);
#endif
