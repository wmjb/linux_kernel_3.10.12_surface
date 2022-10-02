/*
 * drivers/video/tegra/host/gk20a/gk20a.c
 *
 * GK20A Graphics
 *
 * Copyright (c) 2011-2013, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/highmem.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/thermal.h>
#include <asm/cacheflush.h>
#include <linux/debugfs.h>
#include <linux/spinlock.h>
#include <linux/tegra-powergate.h>
#include <linux/fb.h>
#include <linux/suspend.h>

#include <mach/pm_domains.h>

#include "dev.h"
#include "class_ids.h"
#include "bus_client.h"
#include "nvhost_as.h"

#include "gk20a.h"
#include "ctrl_gk20a.h"
#include "hw_mc_gk20a.h"
#include "hw_sim_gk20a.h"
#include "gk20a_scale.h"
#include "gr3d/pod_scaling.h"
#include "dbg_gpu_gk20a.h"

#include "../../../../../arch/arm/mach-tegra/iomap.h"

static inline void set_gk20a(struct platform_device *dev, struct gk20a *gk20a)
{
	nvhost_set_private_data(dev, gk20a);
}

/* TBD: should be able to put in the list below. */
static struct resource gk20a_intr = {
	.start = TEGRA_GK20A_INTR,
	.end   = TEGRA_GK20A_INTR_NONSTALL,
	.flags = IORESOURCE_IRQ,
};

struct resource gk20a_resources_sim[] = {
	{
	.start = TEGRA_GK20A_BAR0_BASE,
	.end   = TEGRA_GK20A_BAR0_BASE + TEGRA_GK20A_BAR0_SIZE - 1,
	.flags = IORESOURCE_MEM,
	},
	{
	.start = TEGRA_GK20A_BAR1_BASE,
	.end   = TEGRA_GK20A_BAR1_BASE + TEGRA_GK20A_BAR1_SIZE - 1,
	.flags = IORESOURCE_MEM,
	},
	{
	.start = TEGRA_GK20A_SIM_BASE,
	.end   = TEGRA_GK20A_SIM_BASE + TEGRA_GK20A_SIM_SIZE - 1,
	.flags = IORESOURCE_MEM,
	},
};

const struct file_operations tegra_gk20a_ctrl_ops = {
	.owner = THIS_MODULE,
	.release = gk20a_ctrl_dev_release,
	.open = gk20a_ctrl_dev_open,
	.unlocked_ioctl = gk20a_ctrl_dev_ioctl,
};

const struct file_operations tegra_gk20a_dbg_gpu_ops = {
	.owner = THIS_MODULE,
	.release        = gk20a_dbg_gpu_dev_release,
	.open           = gk20a_dbg_gpu_dev_open,
	.unlocked_ioctl = gk20a_dbg_gpu_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gk20a_dbg_gpu_dev_ioctl,
#endif
};

/*
 * Note: We use a different 'open' to trigger handling of the profiler session.
 * Most of the code is shared between them...  Though, at some point if the
 * code does get too tangled trying to handle each in the same path we can
 * separate them cleanly.
 */
const struct file_operations tegra_gk20a_prof_gpu_ops = {
	.owner = THIS_MODULE,
	.release        = gk20a_dbg_gpu_dev_release,
	.open           = gk20a_prof_gpu_dev_open,
	.unlocked_ioctl = gk20a_dbg_gpu_dev_ioctl,
	/* .mmap           = gk20a_prof_gpu_dev_mmap,*/
	/*int (*mmap) (struct file *, struct vm_area_struct *);*/
	.compat_ioctl = gk20a_dbg_gpu_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gk20a_dbg_gpu_dev_ioctl,
#endif
};


static inline void sim_writel(struct gk20a *g, u32 r, u32 v)
{
	writel(v, g->sim.regs+r);
}

static inline u32 sim_readl(struct gk20a *g, u32 r)
{
	return readl(g->sim.regs+r);
}

static void kunmap_and_free_iopage(void **kvaddr, struct page **page)
{
	if (*kvaddr) {
		kunmap(*kvaddr);
		*kvaddr = 0;
	}
	if (*page) {
		__free_page(*page);
		*page = 0;
	}
}

static void gk20a_free_sim_support(struct gk20a *g)
{
	/* free sim mappings, bfrs */
	kunmap_and_free_iopage(&g->sim.send_bfr.kvaddr,
			       &g->sim.send_bfr.page);

	kunmap_and_free_iopage(&g->sim.recv_bfr.kvaddr,
			       &g->sim.recv_bfr.page);

	kunmap_and_free_iopage(&g->sim.msg_bfr.kvaddr,
			       &g->sim.msg_bfr.page);
}

static void gk20a_remove_sim_support(struct sim_gk20a *s)
{
	struct gk20a *g = s->g;
	if (g->sim.regs)
		sim_writel(g, sim_config_r(), sim_config_mode_disabled_v());
	gk20a_free_sim_support(g);
}

static int alloc_and_kmap_iopage(struct device *d,
				 void **kvaddr,
				 phys_addr_t *phys,
				 struct page **page)
{
	int err = 0;
	*page = alloc_page(GFP_KERNEL);

	if (!*page) {
		err = -ENOMEM;
		dev_err(d, "couldn't allocate io page\n");
		goto fail;
	}

	*kvaddr = kmap(*page);
	if (!*kvaddr) {
		err = -ENOMEM;
		dev_err(d, "couldn't kmap io page\n");
		goto fail;
	}
	*phys = page_to_phys(*page);
	return 0;

 fail:
	kunmap_and_free_iopage(kvaddr, page);
	return err;

}
/* TBD: strip from released */
static int gk20a_init_sim_support(struct platform_device *dev)
{
	int err = 0;
	struct gk20a *g = get_gk20a(dev);
	struct nvhost_device_data *pdata = nvhost_get_devdata(dev);
	struct device *d = &dev->dev;
	phys_addr_t phys;

	g->sim.g = g;
	g->sim.regs = pdata->aperture[GK20A_SIM_IORESOURCE_MEM];
	if (!g->sim.regs) {
		dev_err(d, "failed to remap gk20a sim regs\n");
		err = -ENXIO;
		goto fail;
	}

	/* allocate sim event/msg buffers */
	err = alloc_and_kmap_iopage(d, &g->sim.send_bfr.kvaddr,
				    &g->sim.send_bfr.phys,
				    &g->sim.send_bfr.page);

	err = err || alloc_and_kmap_iopage(d, &g->sim.recv_bfr.kvaddr,
					   &g->sim.recv_bfr.phys,
					   &g->sim.recv_bfr.page);

	err = err || alloc_and_kmap_iopage(d, &g->sim.msg_bfr.kvaddr,
					   &g->sim.msg_bfr.phys,
					   &g->sim.msg_bfr.page);

	if (!(g->sim.send_bfr.kvaddr && g->sim.recv_bfr.kvaddr &&
	      g->sim.msg_bfr.kvaddr)) {
		dev_err(d, "couldn't allocate all sim buffers\n");
		goto fail;
	}

	/*mark send ring invalid*/
	sim_writel(g, sim_send_ring_r(), sim_send_ring_status_invalid_f());

	/*read get pointer and make equal to put*/
	g->sim.send_ring_put = sim_readl(g, sim_send_get_r());
	sim_writel(g, sim_send_put_r(), g->sim.send_ring_put);

	/*write send ring address and make it valid*/
	/*TBD: work for >32b physmem*/
	phys = g->sim.send_bfr.phys;
	sim_writel(g, sim_send_ring_hi_r(), 0);
	sim_writel(g, sim_send_ring_r(),
		   sim_send_ring_status_valid_f() |
		   sim_send_ring_target_phys_pci_coherent_f() |
		   sim_send_ring_size_4kb_f() |
		   sim_send_ring_addr_lo_f(phys >> PAGE_SHIFT));

	/*repeat for recv ring (but swap put,get as roles are opposite) */
	sim_writel(g, sim_recv_ring_r(), sim_recv_ring_status_invalid_f());

	/*read put pointer and make equal to get*/
	g->sim.recv_ring_get = sim_readl(g, sim_recv_put_r());
	sim_writel(g, sim_recv_get_r(), g->sim.recv_ring_get);

	/*write send ring address and make it valid*/
	/*TBD: work for >32b physmem*/
	phys = g->sim.recv_bfr.phys;
	sim_writel(g, sim_recv_ring_hi_r(), 0);
	sim_writel(g, sim_recv_ring_r(),
		   sim_recv_ring_status_valid_f() |
		   sim_recv_ring_target_phys_pci_coherent_f() |
		   sim_recv_ring_size_4kb_f() |
		   sim_recv_ring_addr_lo_f(phys >> PAGE_SHIFT));

	g->sim.remove_support = gk20a_remove_sim_support;
	return 0;

 fail:
	gk20a_free_sim_support(g);
	return err;
}

static inline u32 sim_msg_header_size(void)
{
	return 24;/*TBD: fix the header to gt this from NV_VGPU_MSG_HEADER*/
}

static inline u32 *sim_msg_bfr(struct gk20a *g, u32 byte_offset)
{
	return (u32 *)(g->sim.msg_bfr.kvaddr + byte_offset);
}

static inline u32 *sim_msg_hdr(struct gk20a *g, u32 byte_offset)
{
	return sim_msg_bfr(g, byte_offset); /*starts at 0*/
}

static inline u32 *sim_msg_param(struct gk20a *g, u32 byte_offset)
{
	/*starts after msg header/cmn*/
	return sim_msg_bfr(g, byte_offset + sim_msg_header_size());
}

static inline void sim_write_hdr(struct gk20a *g, u32 func, u32 size)
{
	/*memset(g->sim.msg_bfr.kvaddr,0,min(PAGE_SIZE,size));*/
	*sim_msg_hdr(g, sim_msg_signature_r()) = sim_msg_signature_valid_v();
	*sim_msg_hdr(g, sim_msg_result_r())    = sim_msg_result_rpc_pending_v();
	*sim_msg_hdr(g, sim_msg_spare_r())     = sim_msg_spare__init_v();
	*sim_msg_hdr(g, sim_msg_function_r())  = func;
	*sim_msg_hdr(g, sim_msg_length_r())    = size + sim_msg_header_size();
}

static inline u32 sim_escape_read_hdr_size(void)
{
	return 12; /*TBD: fix NV_VGPU_SIM_ESCAPE_READ_HEADER*/
}

static u32 *sim_send_ring_bfr(struct gk20a *g, u32 byte_offset)
{
	return (u32 *)(g->sim.send_bfr.kvaddr + byte_offset);
}

static int rpc_send_message(struct gk20a *g)
{
	/* calculations done in units of u32s */
	u32 send_base = sim_send_put_pointer_v(g->sim.send_ring_put) * 2;
	u32 dma_offset = send_base + sim_dma_r()/sizeof(u32);
	u32 dma_hi_offset = send_base + sim_dma_hi_r()/sizeof(u32);

	*sim_send_ring_bfr(g, dma_offset*sizeof(u32)) =
		sim_dma_target_phys_pci_coherent_f() |
		sim_dma_status_valid_f() |
		sim_dma_size_4kb_f() |
		sim_dma_addr_lo_f(g->sim.msg_bfr.phys >> PAGE_SHIFT);

	*sim_send_ring_bfr(g, dma_hi_offset*sizeof(u32)) = 0; /*TBD >32b phys*/

	*sim_msg_hdr(g, sim_msg_sequence_r()) = g->sim.sequence_base++;

	g->sim.send_ring_put = (g->sim.send_ring_put + 2 * sizeof(u32)) %
		PAGE_SIZE;

	__cpuc_flush_dcache_area(g->sim.msg_bfr.kvaddr, PAGE_SIZE);
	__cpuc_flush_dcache_area(g->sim.send_bfr.kvaddr, PAGE_SIZE);
	__cpuc_flush_dcache_area(g->sim.recv_bfr.kvaddr, PAGE_SIZE);

	/* Update the put pointer. This will trap into the host. */
	sim_writel(g, sim_send_put_r(), g->sim.send_ring_put);

	return 0;
}

static inline u32 *sim_recv_ring_bfr(struct gk20a *g, u32 byte_offset)
{
	return (u32 *)(g->sim.recv_bfr.kvaddr + byte_offset);
}

static int rpc_recv_poll(struct gk20a *g)
{
	phys_addr_t recv_phys_addr;

	/* XXX This read is not required (?) */
	/*pVGpu->recv_ring_get = VGPU_REG_RD32(pGpu, NV_VGPU_RECV_GET);*/

	/* Poll the recv ring get pointer in an infinite loop*/
	do {
		g->sim.recv_ring_put = sim_readl(g, sim_recv_put_r());
	} while (g->sim.recv_ring_put == g->sim.recv_ring_get);

	/* process all replies */
	while (g->sim.recv_ring_put != g->sim.recv_ring_get) {
		/* these are in u32 offsets*/
		u32 dma_lo_offset =
			sim_recv_put_pointer_v(g->sim.recv_ring_get)*2 + 0;
		/*u32 dma_hi_offset = dma_lo_offset + 1;*/
		u32 recv_phys_addr_lo =	sim_dma_addr_lo_v(*sim_recv_ring_bfr(g, dma_lo_offset*4));

		/*u32 recv_phys_addr_hi = sim_dma_hi_addr_v(
		      (phys_addr_t)sim_recv_ring_bfr(g,dma_hi_offset*4));*/

		/*TBD >32b phys addr */
		recv_phys_addr = recv_phys_addr_lo << PAGE_SHIFT;

		if (recv_phys_addr != g->sim.msg_bfr.phys) {
			dev_err(dev_from_gk20a(g), "%s Error in RPC reply\n",
				__func__);
			return -1;
		}

		/* Update GET pointer */
		g->sim.recv_ring_get = (g->sim.recv_ring_get + 2*sizeof(u32)) %
			PAGE_SIZE;

		__cpuc_flush_dcache_area(g->sim.msg_bfr.kvaddr, PAGE_SIZE);
		__cpuc_flush_dcache_area(g->sim.send_bfr.kvaddr, PAGE_SIZE);
		__cpuc_flush_dcache_area(g->sim.recv_bfr.kvaddr, PAGE_SIZE);

		sim_writel(g, sim_recv_get_r(), g->sim.recv_ring_get);

		g->sim.recv_ring_put = sim_readl(g, sim_recv_put_r());
	}

	return 0;
}

static int issue_rpc_and_wait(struct gk20a *g)
{
	int err;

	err = rpc_send_message(g);
	if (err) {
		dev_err(dev_from_gk20a(g), "%s failed rpc_send_message\n",
			__func__);
		return err;
	}

	err = rpc_recv_poll(g);
	if (err) {
		dev_err(dev_from_gk20a(g), "%s failed rpc_recv_poll\n",
			__func__);
		return err;
	}

	/* Now check if RPC really succeeded */
	if (*sim_msg_hdr(g, sim_msg_result_r()) != sim_msg_result_success_v()) {
		dev_err(dev_from_gk20a(g), "%s received failed status!\n",
			__func__);
		return -(*sim_msg_hdr(g, sim_msg_result_r()));
	}
	return 0;
}

int gk20a_sim_esc_read(struct gk20a *g, char *path, u32 index, u32 count, u32 *data)
{
	int err;
	size_t pathlen = strlen(path);
	u32 data_offset;

	sim_write_hdr(g, sim_msg_function_sim_escape_read_v(),
		      sim_escape_read_hdr_size());
	*sim_msg_param(g, 0) = index;
	*sim_msg_param(g, 4) = count;
	data_offset = roundup(0xc +  pathlen + 1, sizeof(u32));
	*sim_msg_param(g, 8) = data_offset;
	strcpy((char *)sim_msg_param(g, 0xc), path);

	err = issue_rpc_and_wait(g);

	if (!err)
		memcpy(data, sim_msg_param(g, data_offset), count);
	return err;
}

static irqreturn_t gk20a_intr_isr(int irq, void *dev_id)
{
	struct gk20a *g = dev_id;
	u32 mc_intr_0 = gk20a_readl(g, mc_intr_0_r());

	/* not from gpu when sharing irq with others */
	if (unlikely(!mc_intr_0))
		return IRQ_NONE;

	gk20a_writel(g, mc_intr_en_0_r(),
		mc_intr_en_0_inta_disabled_f());

	/* flush previous write */
	gk20a_readl(g, mc_intr_en_0_r());

	return IRQ_WAKE_THREAD;
}

static irqreturn_t gk20a_intr_thread(int irq, void *dev_id)
{
	struct gk20a *g = dev_id;
	u32 mc_intr_0;

	nvhost_dbg(dbg_intr, "interrupt thread launched");

	mc_intr_0 = gk20a_readl(g, mc_intr_0_r());

	if (mc_intr_0 & mc_intr_0_pgraph_pending_f())
		gk20a_gr_isr(g);
	if (mc_intr_0 & mc_intr_0_pfifo_pending_f())
		gk20a_fifo_isr(g);
	if (mc_intr_0 & mc_intr_0_pmu_pending_f())
		gk20a_pmu_isr(g);
	if (mc_intr_0 & mc_intr_0_priv_ring_pending_f())
		gk20a_priv_ring_isr(g);
	if (mc_intr_0 & mc_intr_0_ltc_pending_f())
		gk20a_mm_ltc_isr(g);
	if (mc_intr_0)
		nvhost_dbg_info("leaving isr with interrupt pending 0x%08x",
				mc_intr_0);

	gk20a_writel(g, mc_intr_en_0_r(),
		mc_intr_en_0_inta_hardware_f());

	/* flush previous write */
	gk20a_readl(g, mc_intr_en_0_r());

	return IRQ_HANDLED;
}

static void gk20a_remove_support(struct platform_device *dev)
{
	struct gk20a *g = get_gk20a(dev);

	/* pmu support should already be removed when driver turns off
	   gpu power rail in prepapre_poweroff */
	if (g->gk20a_cdev.gk20a_cooling_dev)
		thermal_cooling_device_unregister(g->gk20a_cdev.gk20a_cooling_dev);

	if (g->gr.remove_support)
		g->gr.remove_support(&g->gr);

	if (g->fifo.remove_support)
		g->fifo.remove_support(&g->fifo);

	if (g->mm.remove_support)
		g->mm.remove_support(&g->mm);

	if (g->sim.remove_support)
		g->sim.remove_support(&g->sim);

	release_firmware(g->pmu_fw);

	if (g->irq_requested) {
		free_irq(gk20a_intr.start, g);
		g->irq_requested = false;
	}

	/* free mappings to registers, etc*/

	if (g->regs) {
		iounmap(g->regs);
		g->regs = 0;
	}
}

int nvhost_init_gk20a_support(struct platform_device *dev)
{
	int err = 0;
	struct gk20a *g = get_gk20a(dev);
	struct nvhost_device_data *pdata = nvhost_get_devdata(dev);

	g->regs = pdata->aperture[GK20A_BAR0_IORESOURCE_MEM];
	if (!g->regs) {
		dev_err(dev_from_gk20a(g), "failed to remap gk20a registers\n");
		err = -ENXIO;
		goto fail;
	}

	g->bar1 = pdata->aperture[GK20A_BAR1_IORESOURCE_MEM];
	if (!g->bar1) {
		dev_err(dev_from_gk20a(g), "failed to remap gk20a bar1\n");
		err = -ENXIO;
		goto fail;
	}

	err = request_threaded_irq(gk20a_intr.start,
			gk20a_intr_isr, gk20a_intr_thread,
			0, "gk20a", g);
	if (err) {
		dev_err(dev_from_gk20a(g), "failed to request stall interrupt irq @ %lld\n",
			(u64)gk20a_intr.start);
		goto fail;
	}
	g->irq_requested = true;

	if (tegra_cpu_is_asim()) {
		err = gk20a_init_sim_support(dev);
		if (err)
			goto fail;
	}

	mutex_init(&g->dbg_sessions_lock);

	/* nvhost_as alloc_share can be called before gk20a is powered on.
	   It requires mm sw states configured so init mm sw early here. */
	err = gk20a_init_mm_setup_sw(g);
	if (err)
		goto fail;

	/* other inits are deferred until gpu is powered up. */

	g->remove_support = gk20a_remove_support;
	return 0;

 fail:
	gk20a_remove_support(dev);
	return err;
}

int nvhost_gk20a_init(struct platform_device *dev)
{
	nvhost_dbg_fn("");

#ifndef CONFIG_PM_RUNTIME
	nvhost_gk20a_finalize_poweron(dev);
#endif

	if (IS_ENABLED(CONFIG_TEGRA_GK20A_DEVFREQ))
		nvhost_gk20a_scale_hw_init(dev);
	return 0;
}

void nvhost_gk20a_deinit(struct platform_device *dev)
{
	nvhost_dbg_fn("");
#ifndef CONFIG_PM_RUNTIME
	nvhost_gk20a_prepare_poweroff(dev);
#endif
}

static void gk20a_free_hwctx(struct kref *ref)
{
	struct nvhost_hwctx *ctx = container_of(ref, struct nvhost_hwctx, ref);
	nvhost_dbg_fn("");

	nvhost_module_busy(ctx->channel->dev);
	if (ctx->priv)
		gk20a_free_channel(ctx, true);
	nvhost_module_idle(ctx->channel->dev);

	kfree(ctx);
}

static struct nvhost_hwctx *gk20a_alloc_hwctx(struct nvhost_hwctx_handler *h,
					      struct nvhost_channel *ch)
{
	struct nvhost_hwctx *ctx;
	nvhost_dbg_fn("");

	/* it seems odd to be allocating a channel here but the
	 * t20/t30 notion of a channel is mapped on top of gk20a's
	 * channel.  this works because there is only one module
	 * under gk20a's host (gr).
	 */
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->ref);
	ctx->h = h;
	ctx->channel = ch;

	return gk20a_open_channel(ch, ctx);
}

static void gk20a_get_hwctx(struct nvhost_hwctx *hwctx)
{
	nvhost_dbg_fn("");
	kref_get(&hwctx->ref);
}

static void gk20a_put_hwctx(struct nvhost_hwctx *hwctx)
{
	nvhost_dbg_fn("");
	kref_put(&hwctx->ref, gk20a_free_hwctx);
}

static void gk20a_save_push_hwctx(struct nvhost_hwctx *ctx, struct nvhost_cdma *cdma)
{
	nvhost_dbg_fn("");
}

static void gk20a_save_service_hwctx(struct nvhost_hwctx *ctx)
{
	nvhost_dbg_fn("");
}

struct nvhost_hwctx_handler *
    nvhost_gk20a_alloc_hwctx_handler(u32 syncpt, u32 waitbase,
				     struct nvhost_channel *ch)
{

	struct nvhost_hwctx_handler *h;
	nvhost_dbg_fn("");

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return NULL;

	h->alloc = gk20a_alloc_hwctx;
	h->get   = gk20a_get_hwctx;
	h->put   = gk20a_put_hwctx;
	h->save_push = gk20a_save_push_hwctx;
	h->save_service = gk20a_save_service_hwctx;

	return h;
}

int nvhost_gk20a_prepare_poweroff(struct platform_device *dev)
{
	struct gk20a *g = get_gk20a(dev);
	int ret = 0;

	nvhost_dbg_fn("");

	if (!g->power_on)
		return 0;

	ret |= gk20a_channel_suspend(g);

	ret |= gk20a_fifo_suspend(g);
	/* disable elpg before gr suspend */
	ret |= gk20a_pmu_destroy(g);
	ret |= gk20a_gr_suspend(g);
	ret |= gk20a_mm_suspend(g);

	/* Disable GPCPLL */
	ret |= gk20a_suspend_clk_support(g);
	g->power_on = false;

	return ret;
}

int nvhost_gk20a_finalize_poweron(struct platform_device *dev)
{
	struct gk20a *g = get_gk20a(dev);
	int err;

	nvhost_dbg_fn("");

	if (g->power_on)
		return 0;

	g->power_on = true;

	gk20a_writel(g, mc_intr_en_1_r(),
		mc_intr_en_1_inta_disabled_f());

	gk20a_writel(g, mc_intr_en_0_r(),
		mc_intr_en_0_inta_hardware_f());

	gk20a_reset_priv_ring(g);

	/* TBD: move this after graphics init in which blcg/slcg is enabled.
	   This function removes SlowdownOnBoot which applies 32x divider
	   on gpcpll bypass path. The purpose of slowdown is to save power
	   during boot but it also significantly slows down gk20a init on
	   simulation and emulation. We should remove SOB after graphics power
	   saving features (blcg/slcg) are enabled. For now, do it here. */
	err = gk20a_init_clk_support(g);
	if (err)
		nvhost_err(&dev->dev, "failed to init gk20a clk");

	err = gk20a_init_mm_support(g);
	if (err)
		nvhost_err(&dev->dev, "failed to init gk20a mm");

	err = gk20a_init_fifo_support(g);
	if (err)
		nvhost_err(&dev->dev, "failed to init gk20a fifo");

	err = gk20a_init_gr_support(g);
	if (err)
		nvhost_err(&dev->dev, "failed to init gk20a gr");

	err = gk20a_init_pmu_support(g);
	if (err)
		nvhost_err(&dev->dev, "failed to init gk20a pmu");

	err = gk20a_init_therm_support(g);
	if (err)
		nvhost_err(&dev->dev, "failed to init gk20a therm");

	gk20a_channel_resume(g);

	return err;
}

static struct of_device_id tegra_gk20a_of_match[] = {
	{ .compatible = "nvidia,tegra124-gk20a",
		.data = (struct nvhost_device_data *)&tegra_gk20a_info },
	{ },
};

int tegra_gpu_get_max_state(struct thermal_cooling_device *cdev,
		unsigned long *max_state)
{
	struct cooling_device_gk20a *gk20a_gpufreq_device = cdev->devdata;

	*max_state = gk20a_gpufreq_device->gk20a_freq_table_size - 1;
	return 0;
}

int tegra_gpu_get_cur_state(struct thermal_cooling_device *cdev,
		unsigned long *cur_state)
{
	struct cooling_device_gk20a  *gk20a_gpufreq_device = cdev->devdata;

	*cur_state = gk20a_gpufreq_device->gk20a_freq_state;
	return 0;
}

int tegra_gpu_set_cur_state(struct thermal_cooling_device *c_dev,
		unsigned long cur_state)
{
	u32 target_freq;
	struct gk20a *g;
	struct gpufreq_table_data *gpu_cooling_table;
	struct cooling_device_gk20a *gk20a_gpufreq_device = c_dev->devdata;

	BUG_ON(cur_state >= gk20a_gpufreq_device->gk20a_freq_table_size);

	g = container_of(gk20a_gpufreq_device, struct gk20a, gk20a_cdev);

	gpu_cooling_table = tegra_gpufreq_table_get();
	target_freq = gpu_cooling_table[cur_state].frequency;

	/* ensure a query for state will get the proper value */
	gk20a_gpufreq_device->gk20a_freq_state = cur_state;

	gk20a_clk_set_rate(g, target_freq);

	return 0;
}

static struct thermal_cooling_device_ops tegra_gpu_cooling_ops = {
	.get_max_state = tegra_gpu_get_max_state,
	.get_cur_state = tegra_gpu_get_cur_state,
	.set_cur_state = tegra_gpu_set_cur_state,
};

static void gk20a_set_railgating(struct gk20a *g, int new_status)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(g->dev);
	enum pm_qos_flags_status stat;

	mutex_lock(&pdata->lock);
	stat = dev_pm_qos_flags(dev_from_gk20a(g), PM_QOS_FLAG_NO_POWER_OFF);
	if (stat <= PM_QOS_FLAGS_NONE && new_status == FB_BLANK_UNBLANK)
		dev_pm_qos_add_request(dev_from_gk20a(g), &g->no_poweroff_req,
				DEV_PM_QOS_FLAGS, PM_QOS_FLAG_NO_POWER_OFF);
	else if (stat > PM_QOS_FLAGS_NONE && new_status != FB_BLANK_UNBLANK)
		dev_pm_qos_remove_request(&g->no_poweroff_req);
	mutex_unlock(&pdata->lock);
}

static int gk20a_suspend_notifier(struct notifier_block *notifier,
				  unsigned long pm_event, void *data)
{
	struct gk20a *g = container_of(notifier, struct gk20a,
				       system_suspend_notifier);

	if (pm_event == PM_USERSPACE_FROZEN)
		return g->power_on ? NOTIFY_BAD : NOTIFY_OK;

	return NOTIFY_DONE;
}

static int gk20a_fb_notifier(struct notifier_block *notifier,
				  unsigned long pm_event, void *data)
{
	struct gk20a *g = container_of(notifier, struct gk20a,
				       fb_notifier);
	struct fb_event *fb_event = data;

	switch (pm_event) {
	case FB_EVENT_BLANK:
		gk20a_set_railgating(g, *((int *)fb_event->data));
		break;
	}

	return NOTIFY_DONE;
}

static int gk20a_probe(struct platform_device *dev)
{
	struct gk20a *gk20a;
	int err;
	struct nvhost_device_data *pdata = NULL;
	struct cooling_device_gk20a *gpu_cdev = NULL;

	if (dev->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_device(tegra_gk20a_of_match, &dev->dev);
		if (match)
			pdata = (struct nvhost_device_data *)match->data;
	} else
		pdata = (struct nvhost_device_data *)dev->dev.platform_data;

	nvhost_dbg_fn("");
	pdata->pdev = dev;
	mutex_init(&pdata->lock);
	platform_set_drvdata(dev, pdata);

	err = nvhost_client_device_get_resources(dev);
	if (err)
		return err;

	nvhost_module_init(dev);

	gk20a = kzalloc(sizeof(struct gk20a), GFP_KERNEL);
	if (!gk20a) {
		dev_err(&dev->dev, "couldn't allocate gk20a support");
		return -ENOMEM;
	}

	set_gk20a(dev, gk20a);
	gk20a->dev = dev;
	gk20a->host = nvhost_get_host(dev);

	nvhost_init_gk20a_support(dev);

#ifdef CONFIG_PM_GENERIC_DOMAINS
	pdata->pd.name = "gk20a";

	err = nvhost_module_add_domain(&pdata->pd, dev);
#endif

	if (pdata->can_powergate) {
		gk20a->system_suspend_notifier.notifier_call =
			gk20a_suspend_notifier;
		register_pm_notifier(&gk20a->system_suspend_notifier);
		gk20a->fb_notifier.notifier_call = gk20a_fb_notifier;
		fb_register_client(&gk20a->fb_notifier);
	}

	err = nvhost_client_device_init(dev);
	if (err) {
		nvhost_dbg_fn("failed to init client device for %s",
			      dev->name);
		pm_runtime_put(&dev->dev);
		return err;
	}

	err = nvhost_as_init_device(dev);
	if (err) {
		nvhost_dbg_fn("failed to init client address space"
			      " device for %s", dev->name);
		return err;
	}

	gpu_cdev = &gk20a->gk20a_cdev;
	gpu_cdev->gk20a_freq_table_size = tegra_gpufreq_table_size_get();
	gpu_cdev->gk20a_freq_state = 0;
	gpu_cdev->g = gk20a;
	gpu_cdev->gk20a_cooling_dev = thermal_cooling_device_register("gk20a_cdev", gpu_cdev,
					&tegra_gpu_cooling_ops);

	gk20a->gr_idle_timeout_default =
			CONFIG_TEGRA_GRHOST_DEFAULT_TIMEOUT;
	gk20a->timeouts_enabled = true;

	/* Set up initial clock gating settings */
	gk20a->slcg_enabled = true;
	gk20a->blcg_enabled = true;
	gk20a->elcg_enabled = true;
	gk20a->elpg_enabled = true;

	gk20a_create_sysfs(dev);

#ifdef CONFIG_DEBUG_FS
	clk_gk20a_debugfs_init(dev);

	spin_lock_init(&gk20a->debugfs_lock);
	gk20a->mm.ltc_enabled = true;
	gk20a->mm.ltc_enabled_debug = true;
	gk20a->debugfs_ltc_enabled =
			debugfs_create_bool("ltc_enabled", S_IRUGO|S_IWUSR,
				 pdata->debugfs,
				 &gk20a->mm.ltc_enabled_debug);
	gk20a->mm.ltc_enabled_debug = true;
	gk20a->debugfs_gr_idle_timeout_default =
			debugfs_create_u32("gr_idle_timeout_default_us",
					S_IRUGO|S_IWUSR, pdata->debugfs,
					 &gk20a->gr_idle_timeout_default);
	gk20a->debugfs_timeouts_enabled =
			debugfs_create_bool("timeouts_enabled",
					S_IRUGO|S_IWUSR,
					pdata->debugfs,
					&gk20a->timeouts_enabled);
	gk20a_pmu_debugfs_init(dev);
#endif

	return 0;
}

static int __exit gk20a_remove(struct platform_device *dev)
{
	struct gk20a *g = get_gk20a(dev);
	nvhost_dbg_fn("");

	if (g && g->remove_support)
		g->remove_support(dev);

	set_gk20a(dev, 0);
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(g->debugfs_ltc_enabled);
	debugfs_remove(g->debugfs_gr_idle_timeout_default);
	debugfs_remove(g->debugfs_timeouts_enabled);
#endif

	kfree(g);

#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put(&dev->dev);
	pm_runtime_disable(&dev->dev);
#else
	nvhost_module_disable_clk(&dev->dev);
#endif

	return 0;
}

static struct platform_driver gk20a_driver = {
	.probe = gk20a_probe,
	.remove = __exit_p(gk20a_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "gk20a",
#ifdef CONFIG_OF
		.of_match_table = tegra_gk20a_of_match,
#endif
#ifdef CONFIG_PM
		.pm = &nvhost_module_pm_ops,
#endif
	}
};

static int __init gk20a_init(void)
{
		if (tegra_cpu_is_asim()) {
			tegra_gk20a_device.resource = gk20a_resources_sim;
			tegra_gk20a_device.num_resources = 3;
		}
	return platform_driver_register(&gk20a_driver);
}

static void __exit gk20a_exit(void)
{
	platform_driver_unregister(&gk20a_driver);
}

module_init(gk20a_init);
module_exit(gk20a_exit);
