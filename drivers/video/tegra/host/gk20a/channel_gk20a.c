/*
 * drivers/video/tegra/host/gk20a/channel_gk20a.c
 *
 * GK20A Graphics channel
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
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/list.h>
#include <linux/delay.h>
#include <linux/highmem.h> /* need for nvmap.h*/
#include <trace/events/nvhost.h>
#include <linux/scatterlist.h>


#include "../dev.h"
#include "../nvhost_as.h"
#include "debug.h"

#include "gk20a.h"

#include "hw_ram_gk20a.h"
#include "hw_fifo_gk20a.h"
#include "hw_pbdma_gk20a.h"
#include "hw_ccsr_gk20a.h"
#include "hw_ltc_gk20a.h"
#include "chip_support.h"

#define NVMAP_HANDLE_PARAM_SIZE 1

static struct channel_gk20a *acquire_unused_channel(struct fifo_gk20a *f);
static void release_used_channel(struct fifo_gk20a *f, struct channel_gk20a *c);

static int alloc_priv_cmdbuf(struct channel_gk20a *c, u32 size,
			     struct priv_cmd_entry **entry);
static void free_priv_cmdbuf(struct channel_gk20a *c,
			     struct priv_cmd_entry *e);
static void recycle_priv_cmdbuf(struct channel_gk20a *c);

static int channel_gk20a_alloc_priv_cmdbuf(struct channel_gk20a *c);
static void channel_gk20a_free_priv_cmdbuf(struct channel_gk20a *c);

static int channel_gk20a_commit_userd(struct channel_gk20a *c);
static int channel_gk20a_setup_userd(struct channel_gk20a *c);
static int channel_gk20a_setup_ramfc(struct channel_gk20a *c,
			u64 gpfifo_base, u32 gpfifo_entries);

static void channel_gk20a_bind(struct channel_gk20a *ch_gk20a);
static void channel_gk20a_unbind(struct channel_gk20a *ch_gk20a);

static int channel_gk20a_alloc_inst(struct gk20a *g,
				struct channel_gk20a *ch);
static void channel_gk20a_free_inst(struct gk20a *g,
				struct channel_gk20a *ch);

static int channel_gk20a_update_runlist(struct channel_gk20a *c,
					bool add);

static struct channel_gk20a *acquire_unused_channel(struct fifo_gk20a *f)
{
	struct channel_gk20a *ch = NULL;
	int chid;

	mutex_lock(&f->ch_inuse_mutex);
	for (chid = 0; chid < f->num_channels; chid++) {
		if (!f->channel[chid].in_use) {
			f->channel[chid].in_use = true;
			ch = &f->channel[chid];
			break;
		}
	}
	mutex_unlock(&f->ch_inuse_mutex);

	return ch;
}

static void release_used_channel(struct fifo_gk20a *f, struct channel_gk20a *c)
{
	mutex_lock(&f->ch_inuse_mutex);
	f->channel[c->hw_chid].in_use = false;
	mutex_unlock(&f->ch_inuse_mutex);
}

int channel_gk20a_commit_va(struct channel_gk20a *c)
{
	u64 addr;
	u32 addr_lo;
	u32 addr_hi;
	void *inst_ptr;

	nvhost_dbg_fn("");

	inst_ptr = nvhost_memmgr_mmap(c->inst_block.mem.ref);
	if (!inst_ptr)
		return -ENOMEM;

	addr = gk20a_mm_iova_addr(c->vm->pdes.sgt->sgl);
	addr_lo = u64_lo32(addr >> 12);
	addr_hi = u64_hi32(addr);

	nvhost_dbg_info("pde pa=0x%llx addr_lo=0x%x addr_hi=0x%x",
		   (u64)addr, addr_lo, addr_hi);

	mem_wr32(inst_ptr, ram_in_page_dir_base_lo_w(),
		ram_in_page_dir_base_target_vid_mem_f() |
		ram_in_page_dir_base_vol_true_f() |
		ram_in_page_dir_base_lo_f(addr_lo));

	mem_wr32(inst_ptr, ram_in_page_dir_base_hi_w(),
		ram_in_page_dir_base_hi_f(addr_hi));

	mem_wr32(inst_ptr, ram_in_adr_limit_lo_w(),
		 u64_lo32(c->vm->va_limit) | 0xFFF);

	mem_wr32(inst_ptr, ram_in_adr_limit_hi_w(),
		ram_in_adr_limit_hi_f(u64_hi32(c->vm->va_limit)));

	nvhost_memmgr_munmap(c->inst_block.mem.ref, inst_ptr);

	gk20a_mm_l2_invalidate(c->g);

	return 0;
}

static int channel_gk20a_commit_userd(struct channel_gk20a *c)
{
	u32 addr_lo;
	u32 addr_hi;
	void *inst_ptr;

	nvhost_dbg_fn("");

	inst_ptr = nvhost_memmgr_mmap(c->inst_block.mem.ref);
	if (!inst_ptr)
		return -ENOMEM;

	addr_lo = u64_lo32(c->userd_cpu_pa >> ram_userd_base_shift_v());
	addr_hi = u64_hi32(c->userd_cpu_pa);

	nvhost_dbg_info("channel %d : set ramfc userd 0x%16llx",
		c->hw_chid, (u64)c->userd_cpu_pa);

	mem_wr32(inst_ptr, ram_in_ramfc_w() + ram_fc_userd_w(),
		 pbdma_userd_target_vid_mem_f() |
		 pbdma_userd_addr_f(addr_lo));

	mem_wr32(inst_ptr, ram_in_ramfc_w() + ram_fc_userd_hi_w(),
		 pbdma_userd_target_vid_mem_f() |
		 pbdma_userd_hi_addr_f(addr_hi));

	nvhost_memmgr_munmap(c->inst_block.mem.ref, inst_ptr);

	gk20a_mm_l2_invalidate(c->g);

	return 0;
}

static int channel_gk20a_setup_ramfc(struct channel_gk20a *c,
				u64 gpfifo_base, u32 gpfifo_entries)
{
	void *inst_ptr;

	nvhost_dbg_fn("");

	inst_ptr = nvhost_memmgr_mmap(c->inst_block.mem.ref);
	if (!inst_ptr)
		return -ENOMEM;

	memset(inst_ptr, 0, ram_fc_size_val_v());

	mem_wr32(inst_ptr, ram_fc_gp_base_w(),
		pbdma_gp_base_offset_f(
		u64_lo32(gpfifo_base >> pbdma_gp_base_rsvd_s())));

	mem_wr32(inst_ptr, ram_fc_gp_base_hi_w(),
		pbdma_gp_base_hi_offset_f(u64_hi32(gpfifo_base)) |
		pbdma_gp_base_hi_limit2_f(ilog2(gpfifo_entries)));

	mem_wr32(inst_ptr, ram_fc_signature_w(),
		 pbdma_signature_hw_valid_f() | pbdma_signature_sw_zero_f());

	mem_wr32(inst_ptr, ram_fc_formats_w(),
		pbdma_formats_gp_fermi0_f() |
		pbdma_formats_pb_fermi1_f() |
		pbdma_formats_mp_fermi0_f());

	mem_wr32(inst_ptr, ram_fc_pb_header_w(),
		pbdma_pb_header_priv_user_f() |
		pbdma_pb_header_method_zero_f() |
		pbdma_pb_header_subchannel_zero_f() |
		pbdma_pb_header_level_main_f() |
		pbdma_pb_header_first_true_f() |
		pbdma_pb_header_type_inc_f());

	mem_wr32(inst_ptr, ram_fc_subdevice_w(),
		pbdma_subdevice_id_f(1) |
		pbdma_subdevice_status_active_f() |
		pbdma_subdevice_channel_dma_enable_f());

	mem_wr32(inst_ptr, ram_fc_target_w(), pbdma_target_engine_sw_f());

	mem_wr32(inst_ptr, ram_fc_acquire_w(),
		pbdma_acquire_retry_man_2_f() |
		pbdma_acquire_retry_exp_2_f() |
		pbdma_acquire_timeout_exp_max_f() |
		pbdma_acquire_timeout_man_max_f() |
		pbdma_acquire_timeout_en_disable_f());

	mem_wr32(inst_ptr, ram_fc_eng_timeslice_w(),
		fifo_eng_timeslice_timeout_128_f() |
		fifo_eng_timeslice_timescale_3_f() |
		fifo_eng_timeslice_enable_true_f());

	mem_wr32(inst_ptr, ram_fc_pb_timeslice_w(),
		fifo_pb_timeslice_timeout_16_f() |
		fifo_pb_timeslice_timescale_0_f() |
		fifo_pb_timeslice_enable_true_f());

	mem_wr32(inst_ptr, ram_fc_chid_w(), ram_fc_chid_id_f(c->hw_chid));

	/* TBD: alwasy priv mode? */
	mem_wr32(inst_ptr, ram_fc_hce_ctrl_w(),
		 pbdma_hce_ctrl_hce_priv_mode_yes_f());

	nvhost_memmgr_munmap(c->inst_block.mem.ref, inst_ptr);

	gk20a_mm_l2_invalidate(c->g);

	return 0;
}

static int channel_gk20a_setup_userd(struct channel_gk20a *c)
{
	BUG_ON(!c->userd_cpu_va);

	nvhost_dbg_fn("");

	mem_wr32(c->userd_cpu_va, ram_userd_put_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_get_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_ref_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_put_hi_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_ref_threshold_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_gp_top_level_get_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_gp_top_level_get_hi_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_get_hi_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_gp_get_w(), 0);
	mem_wr32(c->userd_cpu_va, ram_userd_gp_put_w(), 0);

	gk20a_mm_l2_invalidate(c->g);

	return 0;
}

static void channel_gk20a_bind(struct channel_gk20a *ch_gk20a)
{
	struct gk20a *g = get_gk20a(ch_gk20a->ch->dev);
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_engine_info_gk20a *engine_info =
		f->engine_info + ENGINE_GR_GK20A;

	u32 inst_ptr = sg_phys(ch_gk20a->inst_block.mem.sgt->sgl)
		>> ram_in_base_shift_v();

	nvhost_dbg_info("bind channel %d inst ptr 0x%08x",
		ch_gk20a->hw_chid, inst_ptr);

	ch_gk20a->bound = true;

	gk20a_writel(g, ccsr_channel_r(ch_gk20a->hw_chid),
		(gk20a_readl(g, ccsr_channel_r(ch_gk20a->hw_chid)) &
		 ~ccsr_channel_runlist_f(~0)) |
		 ccsr_channel_runlist_f(engine_info->runlist_id));

	gk20a_writel(g, ccsr_channel_inst_r(ch_gk20a->hw_chid),
		ccsr_channel_inst_ptr_f(inst_ptr) |
		ccsr_channel_inst_target_vid_mem_f() |
		ccsr_channel_inst_bind_true_f());

	gk20a_writel(g, ccsr_channel_r(ch_gk20a->hw_chid),
		(gk20a_readl(g, ccsr_channel_r(ch_gk20a->hw_chid)) &
		 ~ccsr_channel_enable_set_f(~0)) |
		 ccsr_channel_enable_set_true_f());
}

static void channel_gk20a_unbind(struct channel_gk20a *ch_gk20a)
{
	struct gk20a *g = get_gk20a(ch_gk20a->ch->dev);

	nvhost_dbg_fn("");

	if (ch_gk20a->bound)
		gk20a_writel(g, ccsr_channel_inst_r(ch_gk20a->hw_chid),
			ccsr_channel_inst_ptr_f(0) |
			ccsr_channel_inst_bind_false_f());

	ch_gk20a->bound = false;
}

static int channel_gk20a_alloc_inst(struct gk20a *g,
				struct channel_gk20a *ch)
{
	struct mem_mgr *memmgr = mem_mgr_from_g(g);

	nvhost_dbg_fn("");

	ch->inst_block.mem.ref =
		nvhost_memmgr_alloc(memmgr, ram_in_alloc_size_v(),
				    DEFAULT_ALLOC_ALIGNMENT,
				    DEFAULT_ALLOC_FLAGS,
				    0);

	if (IS_ERR(ch->inst_block.mem.ref)) {
		ch->inst_block.mem.ref = 0;
		goto clean_up;
	}

	ch->inst_block.mem.sgt =
		nvhost_memmgr_sg_table(memmgr, ch->inst_block.mem.ref);

	/* IS_ERR throws a warning here (expecting void *) */
	if (IS_ERR(ch->inst_block.mem.sgt)) {
		ch->inst_block.mem.sgt = NULL;
		goto clean_up;
	}

	nvhost_dbg_info("channel %d inst block physical addr: 0x%16llx",
		ch->hw_chid, (u64)sg_phys(ch->inst_block.mem.sgt->sgl));

	ch->inst_block.mem.size = ram_in_alloc_size_v();

	nvhost_dbg_fn("done");
	return 0;

clean_up:
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	channel_gk20a_free_inst(g, ch);
	return -ENOMEM;
}

static void channel_gk20a_free_inst(struct gk20a *g,
				struct channel_gk20a *ch)
{
	struct mem_mgr *memmgr = mem_mgr_from_g(g);

	nvhost_memmgr_free_sg_table(memmgr, ch->inst_block.mem.ref,
			ch->inst_block.mem.sgt);
	nvhost_memmgr_put(memmgr, ch->inst_block.mem.ref);
	memset(&ch->inst_block, 0, sizeof(struct inst_desc));
}

static int channel_gk20a_update_runlist(struct channel_gk20a *c, bool add)
{
	return gk20a_fifo_update_runlist(c->g, 0, c->hw_chid, add, true);
}

void gk20a_disable_channel_no_update(struct channel_gk20a *ch)
{
	struct nvhost_device_data *pdata = nvhost_get_devdata(ch->g->dev);
	struct nvhost_master *host = host_from_gk20a_channel(ch);

	/* ensure no fences are pending */
	nvhost_syncpt_set_min_eq_max(&host->syncpt,
				     ch->hw_chid + pdata->syncpt_base);

	/* disable channel */
	gk20a_writel(ch->g, ccsr_channel_r(ch->hw_chid),
		     gk20a_readl(ch->g,
		     ccsr_channel_r(ch->hw_chid)) |
		     ccsr_channel_enable_clr_true_f());
}

void gk20a_disable_channel(struct channel_gk20a *ch,
			   bool finish,
			   unsigned long finish_timeout)
{
	if (finish) {
		int err = gk20a_channel_finish(ch, finish_timeout);
		WARN_ON(err);
	}

	/* disable the channel from hw and increment syncpoints */
	gk20a_disable_channel_no_update(ch);

	/* preempt the channel */
	gk20a_fifo_preempt_channel(ch->g, ch->hw_chid);

	/* remove channel from runlist */
	channel_gk20a_update_runlist(ch, false);
}

#if defined(CONFIG_TEGRA_GPU_CYCLE_STATS)

static void gk20a_free_cycle_stats_buffer(struct channel_gk20a *ch)
{
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(ch);
	/* disable existing cyclestats buffer */
	mutex_lock(&ch->cyclestate.cyclestate_buffer_mutex);
	if (ch->cyclestate.cyclestate_buffer_handler) {
		nvhost_memmgr_munmap(ch->cyclestate.cyclestate_buffer_handler,
				ch->cyclestate.cyclestate_buffer);
		nvhost_memmgr_put(memmgr,
				ch->cyclestate.cyclestate_buffer_handler);
		ch->cyclestate.cyclestate_buffer_handler = NULL;
		ch->cyclestate.cyclestate_buffer = NULL;
		ch->cyclestate.cyclestate_buffer_size = 0;
	}
	mutex_unlock(&ch->cyclestate.cyclestate_buffer_mutex);
}

int gk20a_channel_cycle_stats(struct channel_gk20a *ch,
		       struct nvhost_cycle_stats_args *args)
{
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(ch);
	struct mem_handle *handle_ref;
	void *virtual_address;
	u64 cyclestate_buffer_size;
	struct platform_device *dev = ch->ch->dev;

	if (args->nvmap_handle && !ch->cyclestate.cyclestate_buffer_handler) {

		/* set up new cyclestats buffer */
		handle_ref = nvhost_memmgr_get(memmgr,
				args->nvmap_handle, dev);
		if (IS_ERR(handle_ref))
			return PTR_ERR(handle_ref);
		virtual_address = nvhost_memmgr_mmap(handle_ref);
		if (!virtual_address)
			return -ENOMEM;

		nvhost_memmgr_get_param(memmgr, handle_ref,
					NVMAP_HANDLE_PARAM_SIZE,
					&cyclestate_buffer_size);

		ch->cyclestate.cyclestate_buffer_handler = handle_ref;
		ch->cyclestate.cyclestate_buffer = virtual_address;
		ch->cyclestate.cyclestate_buffer_size = cyclestate_buffer_size;
		return 0;

	} else if (!args->nvmap_handle &&
			ch->cyclestate.cyclestate_buffer_handler) {
		gk20a_free_cycle_stats_buffer(ch);
		return 0;

	} else if (!args->nvmap_handle &&
			!ch->cyclestate.cyclestate_buffer_handler) {
		/* no requst from GL */
		return 0;

	} else {
		pr_err("channel already has cyclestats buffer\n");
		return -EINVAL;
	}
}
#endif

void gk20a_free_channel(struct nvhost_hwctx *ctx, bool finish)
{
	struct channel_gk20a *ch = ctx->priv;
	struct gk20a *g = ch->g;
	struct fifo_gk20a *f = &g->fifo;
	struct gr_gk20a *gr = &g->gr;
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(ch);
	struct vm_gk20a *ch_vm = ch->vm;
	unsigned long timeout = gk20a_get_gr_idle_timeout(g);

	nvhost_dbg_fn("");

	if (!ch->bound)
		return;

	if (!gk20a_channel_as_bound(ch))
		goto unbind;

	nvhost_dbg_info("freeing bound channel context, timeout=%ld",
			timeout);

	gk20a_disable_channel(ch, finish, timeout);

	/* release channel ctx */
	gk20a_free_channel_ctx(ch);

	gk20a_gr_flush_channel_tlb(gr);

	memset(&ch->ramfc, 0, sizeof(struct mem_desc_sub));

	/* free gpfifo */
	ch_vm->unmap(ch_vm, ch->gpfifo.gpu_va);
	nvhost_memmgr_munmap(ch->gpfifo.mem.ref, ch->gpfifo.cpu_va);
	gk20a_mm_l2_invalidate(ch->g);

	nvhost_memmgr_put(memmgr, ch->gpfifo.mem.ref);
	memset(&ch->gpfifo, 0, sizeof(struct gpfifo_desc));

#if defined(CONFIG_TEGRA_GPU_CYCLE_STATS)
	gk20a_free_cycle_stats_buffer(ch);
#endif

	ctx->priv = NULL;
	channel_gk20a_free_priv_cmdbuf(ch);

	/* release hwctx binding to the as_share */
	nvhost_as_release_share(ch_vm->as_share, ctx);

unbind:
	channel_gk20a_unbind(ch);
	channel_gk20a_free_inst(g, ch);

	ch->vpr = false;

	/* ALWAYS last */
	release_used_channel(f, ch);
}

struct nvhost_hwctx *gk20a_open_channel(struct nvhost_channel *ch,
					 struct nvhost_hwctx *ctx)
{
	struct gk20a *g = get_gk20a(ch->dev);
	struct fifo_gk20a *f = &g->fifo;
	struct channel_gk20a *ch_gk20a;

	ch_gk20a = acquire_unused_channel(f);
	if (ch_gk20a == NULL) {
		/* TBD: we want to make this virtualizable */
		nvhost_err(dev_from_gk20a(g), "out of hw chids");
		return 0;
	}

	ctx->priv = ch_gk20a;
	ch_gk20a->g = g;
	/* note the ch here is the same for *EVERY* gk20a channel */
	ch_gk20a->ch = ch;
	/* but thre's one hwctx per gk20a channel */
	ch_gk20a->hwctx = ctx;

	if (channel_gk20a_alloc_inst(g, ch_gk20a)) {
		ch_gk20a->in_use = false;
		ctx->priv = 0;
		nvhost_err(dev_from_gk20a(g),
			   "failed to open gk20a channel, out of inst mem");

		return 0;
	}
	channel_gk20a_bind(ch_gk20a);
	ch_gk20a->pid = current->pid;

	/* The channel is *not* runnable at this point. It still needs to have
	 * an address space bound and allocate a gpfifo and grctx. */


	init_waitqueue_head(&ch_gk20a->notifier_wq);
	init_waitqueue_head(&ch_gk20a->semaphore_wq);
	init_waitqueue_head(&ch_gk20a->submit_wq);

	return ctx;
}

#if 0
/* move to debug_gk20a.c ... */
static void dump_gpfifo(struct channel_gk20a *c)
{
	void *inst_ptr;
	u32 chid = c->hw_chid;

	nvhost_dbg_fn("");

	inst_ptr = nvhost_memmgr_mmap(c->inst_block.mem.ref);
	if (!inst_ptr)
		return;

	nvhost_dbg_info("ramfc for channel %d:\n"
		"ramfc: gp_base 0x%08x, gp_base_hi 0x%08x, "
		"gp_fetch 0x%08x, gp_get 0x%08x, gp_put 0x%08x, "
		"pb_fetch 0x%08x, pb_fetch_hi 0x%08x, "
		"pb_get 0x%08x, pb_get_hi 0x%08x, "
		"pb_put 0x%08x, pb_put_hi 0x%08x\n"
		"userd: gp_put 0x%08x, gp_get 0x%08x, "
		"get 0x%08x, get_hi 0x%08x, "
		"put 0x%08x, put_hi 0x%08x\n"
		"pbdma: status 0x%08x, channel 0x%08x, userd 0x%08x, "
		"gp_base 0x%08x, gp_base_hi 0x%08x, "
		"gp_fetch 0x%08x, gp_get 0x%08x, gp_put 0x%08x, "
		"pb_fetch 0x%08x, pb_fetch_hi 0x%08x, "
		"get 0x%08x, get_hi 0x%08x, put 0x%08x, put_hi 0x%08x\n"
		"channel: ccsr_channel 0x%08x",
		chid,
		mem_rd32(inst_ptr, ram_fc_gp_base_w()),
		mem_rd32(inst_ptr, ram_fc_gp_base_hi_w()),
		mem_rd32(inst_ptr, ram_fc_gp_fetch_w()),
		mem_rd32(inst_ptr, ram_fc_gp_get_w()),
		mem_rd32(inst_ptr, ram_fc_gp_put_w()),
		mem_rd32(inst_ptr, ram_fc_pb_fetch_w()),
		mem_rd32(inst_ptr, ram_fc_pb_fetch_hi_w()),
		mem_rd32(inst_ptr, ram_fc_pb_get_w()),
		mem_rd32(inst_ptr, ram_fc_pb_get_hi_w()),
		mem_rd32(inst_ptr, ram_fc_pb_put_w()),
		mem_rd32(inst_ptr, ram_fc_pb_put_hi_w()),
		mem_rd32(c->userd_cpu_va, ram_userd_gp_put_w()),
		mem_rd32(c->userd_cpu_va, ram_userd_gp_get_w()),
		mem_rd32(c->userd_cpu_va, ram_userd_get_w()),
		mem_rd32(c->userd_cpu_va, ram_userd_get_hi_w()),
		mem_rd32(c->userd_cpu_va, ram_userd_put_w()),
		mem_rd32(c->userd_cpu_va, ram_userd_put_hi_w()),
		gk20a_readl(c->g, pbdma_status_r(0)),
		gk20a_readl(c->g, pbdma_channel_r(0)),
		gk20a_readl(c->g, pbdma_userd_r(0)),
		gk20a_readl(c->g, pbdma_gp_base_r(0)),
		gk20a_readl(c->g, pbdma_gp_base_hi_r(0)),
		gk20a_readl(c->g, pbdma_gp_fetch_r(0)),
		gk20a_readl(c->g, pbdma_gp_get_r(0)),
		gk20a_readl(c->g, pbdma_gp_put_r(0)),
		gk20a_readl(c->g, pbdma_pb_fetch_r(0)),
		gk20a_readl(c->g, pbdma_pb_fetch_hi_r(0)),
		gk20a_readl(c->g, pbdma_get_r(0)),
		gk20a_readl(c->g, pbdma_get_hi_r(0)),
		gk20a_readl(c->g, pbdma_put_r(0)),
		gk20a_readl(c->g, pbdma_put_hi_r(0)),
		gk20a_readl(c->g, ccsr_channel_r(chid)));

	nvhost_memmgr_munmap(c->inst_block.mem.ref, inst_ptr);
	gk20a_mm_l2_invalidate(c->g);
}
#endif

/* allocate private cmd buffer.
   used for inserting commands before/after user submitted buffers. */
static int channel_gk20a_alloc_priv_cmdbuf(struct channel_gk20a *c)
{
	struct device *d = dev_from_gk20a(c->g);
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(c);
	struct vm_gk20a *ch_vm = c->vm;
	struct priv_cmd_queue *q = &c->priv_cmd_q;
	struct priv_cmd_entry *e;
	u32 i = 0, size;

	/* Kernel can insert gpfifos before and after user gpfifos.
	   Before user gpfifos, kernel inserts fence_wait, which takes
	   syncpoint_a (2 dwords) + syncpoint_b (2 dwords) = 4 dwords.
	   After user gpfifos, kernel inserts fence_get, which takes
	   wfi (2 dwords) + syncpoint_a (2 dwords) + syncpoint_b (2 dwords)
	   = 6 dwords.
	   Worse case if kernel adds both of them for every user gpfifo,
	   max size of priv_cmdbuf is :
	   (gpfifo entry number * (2 / 3) * (4 + 6) * 4 bytes */
	size = roundup_pow_of_two(
		c->gpfifo.entry_num * 2 * 10 * sizeof(u32) / 3);

	q->mem.ref = nvhost_memmgr_alloc(memmgr,
					 size,
					 DEFAULT_ALLOC_ALIGNMENT,
					 DEFAULT_ALLOC_FLAGS,
					 0);
	if (IS_ERR(q->mem.ref)) {
		nvhost_err(d, "ch %d : failed to allocate"
			   " priv cmd buffer(size: %d bytes)",
			   c->hw_chid, size);
		goto clean_up;
	}
	q->mem.size = size;

	q->base_ptr = (u32 *)nvhost_memmgr_mmap(q->mem.ref);
	if (!q->base_ptr) {
		nvhost_err(d, "ch %d : failed to map cpu va"
			   "for priv cmd buffer", c->hw_chid);
		goto clean_up;
	}

	memset(q->base_ptr, 0, size);

	q->base_gva = ch_vm->map(ch_vm, memmgr,
			q->mem.ref,
			 /*offset_align, flags, kind*/
			0, 0, 0, NULL, false, mem_flag_none);
	if (!q->base_gva) {
		nvhost_err(d, "ch %d : failed to map gpu va"
			   "for priv cmd buffer", c->hw_chid);
		goto clean_up;
	}

	q->size = q->mem.size / sizeof (u32);

	INIT_LIST_HEAD(&q->head);
	INIT_LIST_HEAD(&q->free);

	/* pre-alloc 25% of priv cmdbuf entries and put them on free list */
	for (i = 0; i < q->size / 4; i++) {
		e = kzalloc(sizeof(struct priv_cmd_entry), GFP_KERNEL);
		if (!e) {
			nvhost_err(d, "ch %d: fail to pre-alloc cmd entry",
				c->hw_chid);
			goto clean_up;
		}
		e->pre_alloc = true;
		list_add(&e->list, &q->free);
	}

	return 0;

clean_up:
	channel_gk20a_free_priv_cmdbuf(c);
	return -ENOMEM;
}

static void channel_gk20a_free_priv_cmdbuf(struct channel_gk20a *c)
{
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(c);
	struct vm_gk20a *ch_vm = c->vm;
	struct priv_cmd_queue *q = &c->priv_cmd_q;
	struct priv_cmd_entry *e;
	struct list_head *pos, *tmp, *head;

	if (q->size == 0)
		return;

	ch_vm->unmap(ch_vm, q->base_gva);
	nvhost_memmgr_munmap(q->mem.ref, q->base_ptr);
	nvhost_memmgr_put(memmgr, q->mem.ref);

	/* free used list */
	head = &q->head;
	list_for_each_safe(pos, tmp, head) {
		e = container_of(pos, struct priv_cmd_entry, list);
		free_priv_cmdbuf(c, e);
	}

	/* free free list */
	head = &q->free;
	list_for_each_safe(pos, tmp, head) {
		e = container_of(pos, struct priv_cmd_entry, list);
		e->pre_alloc = false;
		free_priv_cmdbuf(c, e);
	}

	memset(q, 0, sizeof(struct priv_cmd_queue));
}

/* allocate a cmd buffer with given size. size is number of u32 entries */
static int alloc_priv_cmdbuf(struct channel_gk20a *c, u32 orig_size,
			     struct priv_cmd_entry **entry)
{
	struct priv_cmd_queue *q = &c->priv_cmd_q;
	struct priv_cmd_entry *e;
	struct list_head *node;
	u32 free_count;
	u32 size = orig_size;
	bool no_retry = false;

	nvhost_dbg_fn("size %d", orig_size);

	*entry = NULL;

	/* if free space in the end is less than requested, increase the size
	 * to make the real allocated space start from beginning. */
	if (q->put + size > q->size)
		size = orig_size + (q->size - q->put);

	nvhost_dbg_info("ch %d: priv cmd queue get:put %d:%d",
			c->hw_chid, q->get, q->put);

TRY_AGAIN:
	free_count = (q->size - (q->put - q->get) - 1) % q->size;

	if (size > free_count) {
		if (!no_retry) {
			recycle_priv_cmdbuf(c);
			no_retry = true;
			goto TRY_AGAIN;
		} else
			return -EAGAIN;
	}

	if (unlikely(list_empty(&q->free))) {

		nvhost_dbg_info("ch %d: run out of pre-alloc entries",
			c->hw_chid);

		e = kzalloc(sizeof(struct priv_cmd_entry), GFP_KERNEL);
		if (!e) {
			nvhost_err(dev_from_gk20a(c->g),
				"ch %d: fail to allocate priv cmd entry",
				c->hw_chid);
			return -ENOMEM;
		}
	} else  {
		node = q->free.next;
		list_del(node);
		e = container_of(node, struct priv_cmd_entry, list);
	}

	e->size = orig_size;
	e->gp_get = c->gpfifo.get;
	e->gp_put = c->gpfifo.put;
	e->gp_wrap = c->gpfifo.wrap;

	/* if we have increased size to skip free space in the end, set put
	   to beginning of cmd buffer (0) + size */
	if (size != orig_size) {
		e->ptr = q->base_ptr;
		e->gva = q->base_gva;
		q->put = orig_size;
	} else {
		e->ptr = q->base_ptr + q->put;
		e->gva = q->base_gva + q->put * sizeof(u32);
		q->put = (q->put + orig_size) & (q->size - 1);
	}

	/* we already handled q->put + size > q->size so BUG_ON this */
	BUG_ON(q->put > q->size);

	/* add new entry to head since we free from head */
	list_add(&e->list, &q->head);

	*entry = e;

	nvhost_dbg_fn("done");

	return 0;
}

/* Don't call this to free an explict cmd entry.
 * It doesn't update priv_cmd_queue get/put */
static void free_priv_cmdbuf(struct channel_gk20a *c,
			     struct priv_cmd_entry *e)
{
	struct priv_cmd_queue *q = &c->priv_cmd_q;

	if (!e)
		return;

	list_del(&e->list);

	if (unlikely(!e->pre_alloc))
		kfree(e);
	else {
		memset(e, 0, sizeof(struct priv_cmd_entry));
		e->pre_alloc = true;
		list_add(&e->list, &q->free);
	}
}

/* free entries if they're no longer being used */
static void recycle_priv_cmdbuf(struct channel_gk20a *c)
{
	struct priv_cmd_queue *q = &c->priv_cmd_q;
	struct priv_cmd_entry *e, *tmp;
	struct list_head *head = &q->head;
	bool wrap_around, found = false;

	nvhost_dbg_fn("");

	/* Find the most recent free entry. Free it and everything before it */
	list_for_each_entry(e, head, list) {

		nvhost_dbg_info("ch %d: cmd entry get:put:wrap %d:%d:%d "
			"curr get:put:wrap %d:%d:%d",
			c->hw_chid, e->gp_get, e->gp_put, e->gp_wrap,
			c->gpfifo.get, c->gpfifo.put, c->gpfifo.wrap);

		wrap_around = (c->gpfifo.wrap != e->gp_wrap);
		if (e->gp_get < e->gp_put) {
			if (c->gpfifo.get >= e->gp_put ||
			    wrap_around) {
				found = true;
				break;
			} else
				e->gp_get = c->gpfifo.get;
		} else if (e->gp_get > e->gp_put) {
			if (wrap_around &&
			    c->gpfifo.get >= e->gp_put) {
				found = true;
				break;
			} else
				e->gp_get = c->gpfifo.get;
		}
	}

	if (found)
		q->get = (e->ptr - q->base_ptr) + e->size;
	else {
		nvhost_dbg_info("no free entry recycled");
		return;
	}

	list_for_each_entry_safe_continue(e, tmp, head, list) {
		free_priv_cmdbuf(c, e);
	}

	nvhost_dbg_fn("done");
}


int gk20a_alloc_channel_gpfifo(struct channel_gk20a *c,
			       struct nvhost_alloc_gpfifo_args *args)
{
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(c);
	struct gk20a *g = c->g;
	struct nvhost_device_data *pdata = nvhost_get_devdata(g->dev);
	struct device *d = dev_from_gk20a(g);
	struct vm_gk20a *ch_vm;
	u32 gpfifo_size;
	u32 ret;

	/* Kernel can insert one extra gpfifo entry before user submitted gpfifos
	   and another one after, for internal usage. Triple the requested size. */
	gpfifo_size = roundup_pow_of_two(args->num_entries * 3);

	if (args->flags & NVHOST_ALLOC_GPFIFO_FLAGS_VPR_ENABLED)
		c->vpr = true;

	/* an address space needs to have been bound at this point.   */
	if (!gk20a_channel_as_bound(c)) {
		nvhost_err(d,
			    "not bound to an address space at time of gpfifo"
			    " allocation.  Attempting to create and bind to"
			    " one...");
		return -EINVAL;
	}
	ch_vm = c->vm;

	c->cmds_pending = false;

	c->last_submit_fence.valid        = false;
	c->last_submit_fence.syncpt_value = 0;
	c->last_submit_fence.syncpt_id    = c->hw_chid + pdata->syncpt_base;

	c->ramfc.offset = 0;
	c->ramfc.size = ram_in_ramfc_s() / 8;

	if (c->gpfifo.mem.ref) {
		nvhost_err(d, "channel %d :"
			   "gpfifo already allocated", c->hw_chid);
		return -EEXIST;
	}

	c->gpfifo.mem.ref =
		nvhost_memmgr_alloc(memmgr,
				    gpfifo_size * sizeof(struct gpfifo),
				    DEFAULT_ALLOC_ALIGNMENT,
				    DEFAULT_ALLOC_FLAGS,
				    0);
	if (IS_ERR(c->gpfifo.mem.ref)) {
		nvhost_err(d, "channel %d :"
			   " failed to allocate gpfifo (size: %d bytes)",
			   c->hw_chid, gpfifo_size);
		c->gpfifo.mem.ref = 0;
		return PTR_ERR(c->gpfifo.mem.ref);
	}
	c->gpfifo.entry_num = gpfifo_size;

	c->gpfifo.cpu_va =
		(struct gpfifo *)nvhost_memmgr_mmap(c->gpfifo.mem.ref);
	if (!c->gpfifo.cpu_va)
		goto clean_up;

	c->gpfifo.get = c->gpfifo.put = 0;

	c->gpfifo.gpu_va = ch_vm->map(ch_vm, memmgr,
				c->gpfifo.mem.ref,
				/*offset_align, flags, kind*/
				0, 0, 0, NULL, false, mem_flag_none);
	if (!c->gpfifo.gpu_va) {
		nvhost_err(d, "channel %d : failed to map"
			   " gpu_va for gpfifo", c->hw_chid);
		goto clean_up;
	}

	nvhost_dbg_info("channel %d : gpfifo_base 0x%016llx, size %d",
		c->hw_chid, c->gpfifo.gpu_va, c->gpfifo.entry_num);

	channel_gk20a_setup_ramfc(c, c->gpfifo.gpu_va, c->gpfifo.entry_num);

	channel_gk20a_setup_userd(c);
	channel_gk20a_commit_userd(c);

	gk20a_mm_l2_invalidate(c->g);

	/* TBD: setup engine contexts */

	ret = channel_gk20a_alloc_priv_cmdbuf(c);
	if (ret)
		goto clean_up;

	ret = channel_gk20a_update_runlist(c, true);
	if (ret)
		goto clean_up;

	nvhost_dbg_fn("done");
	return 0;

clean_up:
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	ch_vm->unmap(ch_vm, c->gpfifo.gpu_va);
	nvhost_memmgr_munmap(c->gpfifo.mem.ref, c->gpfifo.cpu_va);
	nvhost_memmgr_put(memmgr, c->gpfifo.mem.ref);
	memset(&c->gpfifo, 0, sizeof(struct gpfifo_desc));
	return -ENOMEM;
}

static inline int wfi_cmd_size(void)
{
	return 2;
}
void add_wfi_cmd(struct priv_cmd_entry *cmd, int *i)
{
	/* wfi */
	cmd->ptr[(*i)++] = 0x2001001E;
	/* handle, ignored */
	cmd->ptr[(*i)++] = 0x00000000;
}

static inline bool check_gp_put(struct gk20a *g,
				struct channel_gk20a *c)
{
	u32 put;
	/* gp_put changed unexpectedly since last update? */
	put = gk20a_bar1_readl(g,
	       c->userd_gpu_va + 4 * ram_userd_gp_put_w());
	if (c->gpfifo.put != put) {
		/*TBD: BUG_ON/teardown on this*/
		nvhost_err(dev_from_gk20a(g), "gp_put changed unexpectedly "
			   "since last update");
		c->gpfifo.put = put;
		return false; /* surprise! */
	}
	return true; /* checked out ok */
}

/* Update with this periodically to determine how the gpfifo is draining. */
static inline u32 update_gp_get(struct gk20a *g,
				struct channel_gk20a *c)
{
	u32 new_get = gk20a_bar1_readl(g,
		c->userd_gpu_va + sizeof(u32) * ram_userd_gp_get_w());
	if (new_get < c->gpfifo.get)
		c->gpfifo.wrap = !c->gpfifo.wrap;
	c->gpfifo.get = new_get;
	return new_get;
}

static inline u32 gp_free_count(struct channel_gk20a *c)
{
	return (c->gpfifo.entry_num - (c->gpfifo.put - c->gpfifo.get) - 1) %
		c->gpfifo.entry_num;
}

/* Issue a syncpoint increment *preceded* by a wait-for-idle
 * command.  All commands on the channel will have been
 * consumed at the time the fence syncpoint increment occurs.
 */
int gk20a_channel_submit_wfi_fence(struct gk20a *g,
				   struct channel_gk20a *c,
				   struct nvhost_syncpt *sp,
				   struct nvhost_fence *fence)
{
	struct priv_cmd_entry *cmd = NULL;
	int cmd_size, j = 0;
	u32 free_count;
	int err;

	cmd_size =  4 + wfi_cmd_size();

	update_gp_get(g, c);
	free_count = gp_free_count(c);
	if (unlikely(!free_count)) {
		nvhost_err(dev_from_gk20a(g),
			   "not enough gpfifo space");
		return -EAGAIN;
	}

	err = alloc_priv_cmdbuf(c, cmd_size, &cmd);
	if (unlikely(err)) {
		nvhost_err(dev_from_gk20a(g),
			   "not enough priv cmd buffer space");
		return err;
	}

	fence->value = nvhost_syncpt_incr_max(sp, fence->syncpt_id, 1);

	c->last_submit_fence.valid        = true;
	c->last_submit_fence.syncpt_value = fence->value;
	c->last_submit_fence.syncpt_id    = fence->syncpt_id;
	c->last_submit_fence.wfi          = true;

	trace_nvhost_ioctl_ctrl_syncpt_incr(fence->syncpt_id);


	add_wfi_cmd(cmd, &j);

	/* syncpoint_a */
	cmd->ptr[j++] = 0x2001001C;
	/* payload, ignored */
	cmd->ptr[j++] = 0;
	/* syncpoint_b */
	cmd->ptr[j++] = 0x2001001D;
	/* syncpt_id, incr */
	cmd->ptr[j++] = (fence->syncpt_id << 8) | 0x1;

	c->gpfifo.cpu_va[c->gpfifo.put].entry0 = u64_lo32(cmd->gva);
	c->gpfifo.cpu_va[c->gpfifo.put].entry1 = u64_hi32(cmd->gva) |
		pbdma_gp_entry1_length_f(cmd->size);

	c->gpfifo.put = (c->gpfifo.put + 1) & (c->gpfifo.entry_num - 1);

	/* save gp_put */
	cmd->gp_put = c->gpfifo.put;

	gk20a_bar1_writel(g,
		c->userd_gpu_va + 4 * ram_userd_gp_put_w(),
		c->gpfifo.put);

	nvhost_dbg_info("post-submit put %d, get %d, size %d",
		c->gpfifo.put, c->gpfifo.get, c->gpfifo.entry_num);

	return 0;
}

static u32 get_gp_free_count(struct channel_gk20a *c)
{
	update_gp_get(c->g, c);
	return gp_free_count(c);
}

static void trace_write_pushbuffer(struct channel_gk20a *c, struct gpfifo *g)
{
	void *mem = NULL;
	unsigned int words;
	u64 offset;
	struct mem_handle *r = NULL;

	if (nvhost_debug_trace_cmdbuf) {
		u64 gpu_va = (u64)g->entry0 |
			(u64)((u64)pbdma_gp_entry1_get_hi_v(g->entry1) << 32);
		struct mem_mgr *memmgr = NULL;
		int err;

		words = pbdma_gp_entry1_length_v(g->entry1);
		err = c->vm->find_buffer(c->vm, gpu_va, &memmgr, &r, &offset);
		if (!err)
			mem = nvhost_memmgr_mmap(r);
	}

	if (mem) {
		u32 i;
		/*
		 * Write in batches of 128 as there seems to be a limit
		 * of how much you can output to ftrace at once.
		 */
		for (i = 0; i < words; i += TRACE_MAX_LENGTH) {
			trace_nvhost_cdma_push_gather(
				c->ch->dev->name,
				0,
				min(words - i, TRACE_MAX_LENGTH),
				offset + i * sizeof(u32),
				mem);
		}
		nvhost_memmgr_munmap(r, mem);
	}
}

static int gk20a_channel_add_job(struct channel_gk20a *c,
				 struct nvhost_fence *fence)
{
	struct vm_gk20a *vm = c->vm;
	struct channel_gk20a_job *job = NULL;
	struct mapped_buffer_node **mapped_buffers = NULL;
	int err = 0, num_mapped_buffers;

	err = vm->get_buffers(vm, &mapped_buffers, &num_mapped_buffers);
	if (err)
		return err;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		vm->put_buffers(vm, mapped_buffers, num_mapped_buffers);
		return -ENOMEM;
	}

	job->num_mapped_buffers = num_mapped_buffers;
	job->mapped_buffers = mapped_buffers;
	job->fence = *fence;

	mutex_lock(&c->jobs_lock);
	list_add_tail(&job->list, &c->jobs);
	mutex_unlock(&c->jobs_lock);

	return 0;
}

void gk20a_channel_update(struct channel_gk20a *c)
{
	struct gk20a *g = c->g;
	struct nvhost_syncpt *sp = syncpt_from_gk20a(g);
	struct vm_gk20a *vm = c->vm;
	struct channel_gk20a_job *job, *n;

	mutex_lock(&c->jobs_lock);
	list_for_each_entry_safe(job, n, &c->jobs, list) {
		bool completed = nvhost_syncpt_is_expired(sp,
			job->fence.syncpt_id, job->fence.value);
		if (!completed)
			break;

		vm->put_buffers(vm, job->mapped_buffers,
				job->num_mapped_buffers);
		list_del_init(&job->list);
		kfree(job);
	}
	mutex_unlock(&c->jobs_lock);
}
#ifdef CONFIG_DEBUG_FS
static void gk20a_sync_debugfs(struct gk20a *g)
{
	u32 reg_f = ltc_ltcs_ltss_tstg_set_mgmt_2_l2_bypass_mode_enabled_f();
	spin_lock(&g->debugfs_lock);
	if (g->mm.ltc_enabled != g->mm.ltc_enabled_debug) {
		u32 reg = gk20a_readl(g, ltc_ltcs_ltss_tstg_set_mgmt_2_r());
		if (g->mm.ltc_enabled_debug)
			/* bypass disabled (normal caching ops)*/
			reg &= ~reg_f;
		else
			/* bypass enabled (no caching) */
			reg |= reg_f;

		gk20a_writel(g, ltc_ltcs_ltss_tstg_set_mgmt_2_r(), reg);
		g->mm.ltc_enabled = g->mm.ltc_enabled_debug;
	}
	spin_unlock(&g->debugfs_lock);
}
#endif

int gk20a_submit_channel_gpfifo(struct channel_gk20a *c,
				struct nvhost_gpfifo *gpfifo,
				u32 num_entries,
				struct nvhost_fence *fence,
				u32 flags)
{
	struct gk20a *g = c->g;
	struct nvhost_device_data *pdata = nvhost_get_devdata(g->dev);
	struct device *d = dev_from_gk20a(g);
	struct nvhost_syncpt *sp = syncpt_from_gk20a(g);
	u32 i, incr_id = ~0, wait_id = ~0, wait_value = 0;
	u32 err = 0;
	int incr_cmd_size;
	bool wfi_cmd;
	struct priv_cmd_entry *wait_cmd = NULL;
	struct priv_cmd_entry *incr_cmd = NULL;
	/* we might need two extra gpfifo entries - one for syncpoint
	 * wait and one for syncpoint increment */
	const int extra_entries = 2;

	if ((flags & (NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_WAIT |
		      NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_GET)) &&
	    !fence)
		return -EINVAL;
#ifdef CONFIG_DEBUG_FS
	/* update debug settings */
	gk20a_sync_debugfs(g);
#endif

	nvhost_dbg_info("channel %d", c->hw_chid);

	trace_nvhost_channel_submit_gpfifo(c->ch->dev->name,
					   c->hw_chid,
					   num_entries,
					   flags,
					   fence->syncpt_id, fence->value,
					   c->hw_chid + pdata->syncpt_base);
	check_gp_put(g, c);
	update_gp_get(g, c);

	nvhost_dbg_info("pre-submit put %d, get %d, size %d",
		c->gpfifo.put, c->gpfifo.get, c->gpfifo.entry_num);

	/* If the caller has requested a fence "get" then we need to be
	 * sure the fence represents work completion.  In that case
	 * issue a wait-for-idle before the syncpoint increment.
	 */
	wfi_cmd = !!(flags & NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_GET);

	/* Invalidate tlb if it's dirty...                                   */
	/* TBD: this should be done in the cmd stream, not with PRIs.        */
	/* We don't know what context is currently running...                */
	/* Note also: there can be more than one context associated with the */
	/* address space (vm).   */
	c->vm->tlb_inval(c->vm);

	/* Make sure we have enough space for gpfifo entries. If not,
	 * wait for signals from completed submits */
	if (gp_free_count(c) < num_entries + extra_entries) {
		err = wait_event_interruptible(c->submit_wq,
			get_gp_free_count(c) >= num_entries + extra_entries);
	}

	if (err) {
		nvhost_err(d, "not enough gpfifo space");
		err = -EAGAIN;
		goto clean_up;
	}

	/* optionally insert syncpt wait in the beginning of gpfifo submission
	   when user requested and the wait hasn't expired.
	*/

	/* validate that the id makes sense, elide if not */
	/* the only reason this isn't being unceremoniously killed is to
	 * keep running some tests which trigger this condition*/
	if ((flags & NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_WAIT) &&
	    ((fence->syncpt_id < 0) ||
	     (fence->syncpt_id >= nvhost_syncpt_nb_pts(sp)))) {
		dev_warn(d, "invalid wait id in gpfifo submit, elided");
		flags &= ~NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_WAIT;
	}

	if ((flags & NVHOST_SUBMIT_GPFIFO_FLAGS_FENCE_WAIT) &&
	    !nvhost_syncpt_is_expired(sp, fence->syncpt_id, fence->value)) {
		alloc_priv_cmdbuf(c, 4, &wait_cmd);
		if (wait_cmd == NULL) {
			nvhost_err(d, "not enough priv cmd buffer space");
			err = -EAGAIN;
			goto clean_up;
		}
	}

	/* always insert syncpt increment at end of gpfifo submission
	   to keep track of method completion for idle railgating */
	/* TODO: we need to find a way to get rid of these wfi on every
	 * submission...
	 */
	incr_cmd_size = 4;
	if (wfi_cmd)
		incr_cmd_size += wfi_cmd_size();
	alloc_priv_cmdbuf(c, incr_cmd_size, &incr_cmd);
	if (incr_cmd == NULL) {
		nvhost_err(d, "not enough priv cmd buffer space");
		err = -EAGAIN;
		goto clean_up;
	}

	if (wait_cmd) {
		wait_id = fence->syncpt_id;
		wait_value = fence->value;
		/* syncpoint_a */
		wait_cmd->ptr[0] = 0x2001001C;
		/* payload */
		wait_cmd->ptr[1] = fence->value;
		/* syncpoint_b */
		wait_cmd->ptr[2] = 0x2001001D;
		/* syncpt_id, switch_en, wait */
		wait_cmd->ptr[3] = (wait_id << 8) | 0x10;

		c->gpfifo.cpu_va[c->gpfifo.put].entry0 =
			u64_lo32(wait_cmd->gva);
		c->gpfifo.cpu_va[c->gpfifo.put].entry1 =
			u64_hi32(wait_cmd->gva) |
			pbdma_gp_entry1_length_f(wait_cmd->size);
		trace_write_pushbuffer(c, &c->gpfifo.cpu_va[c->gpfifo.put]);

		c->gpfifo.put = (c->gpfifo.put + 1) &
			(c->gpfifo.entry_num - 1);

		/* save gp_put */
		wait_cmd->gp_put = c->gpfifo.put;
	}

	for (i = 0; i < num_entries; i++) {
		c->gpfifo.cpu_va[c->gpfifo.put].entry0 =
			gpfifo[i].entry0; /* cmd buf va low 32 */
		c->gpfifo.cpu_va[c->gpfifo.put].entry1 =
			gpfifo[i].entry1; /* cmd buf va high 32 | words << 10 */
		trace_write_pushbuffer(c, &c->gpfifo.cpu_va[c->gpfifo.put]);
		c->gpfifo.put = (c->gpfifo.put + 1) &
			(c->gpfifo.entry_num - 1);
	}

	if (incr_cmd) {
		int j = 0;
		incr_id = c->hw_chid + pdata->syncpt_base;
		fence->syncpt_id = incr_id;
		fence->value     = nvhost_syncpt_incr_max(sp, incr_id, 1);

		c->last_submit_fence.valid        = true;
		c->last_submit_fence.syncpt_value = fence->value;
		c->last_submit_fence.syncpt_id    = fence->syncpt_id;
		c->last_submit_fence.wfi          = wfi_cmd;

		trace_nvhost_ioctl_ctrl_syncpt_incr(fence->syncpt_id);
		if (wfi_cmd)
			add_wfi_cmd(incr_cmd, &j);
		/* syncpoint_a */
		incr_cmd->ptr[j++] = 0x2001001C;
		/* payload, ignored */
		incr_cmd->ptr[j++] = 0;
		/* syncpoint_b */
		incr_cmd->ptr[j++] = 0x2001001D;
		/* syncpt_id, incr */
		incr_cmd->ptr[j++] = (fence->syncpt_id << 8) | 0x1;

		c->gpfifo.cpu_va[c->gpfifo.put].entry0 =
			u64_lo32(incr_cmd->gva);
		c->gpfifo.cpu_va[c->gpfifo.put].entry1 =
			u64_hi32(incr_cmd->gva) |
			pbdma_gp_entry1_length_f(incr_cmd->size);
		trace_write_pushbuffer(c, &c->gpfifo.cpu_va[c->gpfifo.put]);

		c->gpfifo.put = (c->gpfifo.put + 1) &
			(c->gpfifo.entry_num - 1);

		/* save gp_put */
		incr_cmd->gp_put = c->gpfifo.put;
	}

	/* Invalidate tlb if it's dirty...                                   */
	/* TBD: this should be done in the cmd stream, not with PRIs.        */
	/* We don't know what context is currently running...                */
	/* Note also: there can be more than one context associated with the */
	/* address space (vm).   */
	c->vm->tlb_inval(c->vm);

	trace_nvhost_channel_submitted_gpfifo(c->ch->dev->name,
					   c->hw_chid,
					   num_entries,
					   flags,
					   wait_id, wait_value,
					   incr_id, fence->value);


	/* TODO! Check for errors... */
	gk20a_channel_add_job(c, fence);

	c->cmds_pending = true;
	gk20a_bar1_writel(g,
		c->userd_gpu_va + 4 * ram_userd_gp_put_w(),
		c->gpfifo.put);

	nvhost_dbg_info("post-submit put %d, get %d, size %d",
		c->gpfifo.put, c->gpfifo.get, c->gpfifo.entry_num);

	nvhost_dbg_fn("done");
	return 0;

clean_up:
	nvhost_dbg(dbg_fn | dbg_err, "fail");
	free_priv_cmdbuf(c, wait_cmd);
	free_priv_cmdbuf(c, incr_cmd);
	return err;
}

void gk20a_remove_channel_support(struct channel_gk20a *c)
{

}

int gk20a_init_channel_support(struct gk20a *g, u32 chid)
{
	struct channel_gk20a *c = g->fifo.channel+chid;
	c->g = g;
	c->in_use = false;
	c->hw_chid = chid;
	c->bound = false;
	c->remove_support = gk20a_remove_channel_support;
	mutex_init(&c->jobs_lock);
	INIT_LIST_HEAD(&c->jobs);
#if defined(CONFIG_TEGRA_GPU_CYCLE_STATS)
	mutex_init(&c->cyclestate.cyclestate_buffer_mutex);
#endif
	mutex_init(&c->dbg_s_lock);
	return 0;
}

int gk20a_channel_init(struct nvhost_channel *ch,
		       struct nvhost_master *host, int index)
{
	return 0;
}

int gk20a_channel_alloc_obj(struct nvhost_channel *channel,
			u32 class_num,
			u32 *obj_id,
			u32 vaspace_share)
{
	nvhost_dbg_fn("");
	return 0;
}

int gk20a_channel_free_obj(struct nvhost_channel *channel, u32 obj_id)
{
	nvhost_dbg_fn("");
	return 0;
}

int gk20a_channel_finish(struct channel_gk20a *ch, unsigned long timeout)
{
	struct nvhost_syncpt *sp = syncpt_from_gk20a(ch->g);
	struct nvhost_device_data *pdata = nvhost_get_devdata(ch->g->dev);
	struct nvhost_fence fence;
	int err = 0;

	if (!ch->cmds_pending)
		return 0;

	if (!(ch->last_submit_fence.valid && ch->last_submit_fence.wfi)) {
		nvhost_dbg_fn("issuing wfi, incr to finish the channel");
		fence.syncpt_id = ch->hw_chid + pdata->syncpt_base;
		err = gk20a_channel_submit_wfi_fence(ch->g, ch,
						     sp, &fence);
	}
	if (err)
		return err;

	BUG_ON(!(ch->last_submit_fence.valid && ch->last_submit_fence.wfi));

	nvhost_dbg_fn("waiting for channel to finish syncpt:%d val:%d",
		      ch->last_submit_fence.syncpt_id,
		      ch->last_submit_fence.syncpt_value);
	err = nvhost_syncpt_wait_timeout(sp,
					 ch->last_submit_fence.syncpt_id,
					 ch->last_submit_fence.syncpt_value,
					 timeout, &fence.value, NULL, false);
	if (WARN_ON(err))
		dev_warn(dev_from_gk20a(ch->g),
			 "timed out waiting for gk20a channel to finish");
	else
		ch->cmds_pending = false;

	return err;
}

int gk20a_channel_wait(struct channel_gk20a *ch,
		       struct nvhost_wait_args *args)
{
	struct device *d = dev_from_gk20a(ch->g);
	struct platform_device *dev = ch->ch->dev;
	struct mem_mgr *memmgr = gk20a_channel_mem_mgr(ch);
	struct mem_handle *handle_ref;
	struct notification *notif;
	struct timespec tv;
	u64 jiffies;
	ulong id;
	u32 offset;
	unsigned long timeout;
	int remain, ret = 0;

	nvhost_dbg_fn("");

	if (args->timeout == NVHOST_NO_TIMEOUT)
		timeout = MAX_SCHEDULE_TIMEOUT;
	else
		timeout = (u32)msecs_to_jiffies(args->timeout);

	switch (args->type) {
	case NVHOST_WAIT_TYPE_NOTIFIER:
		id = args->condition.notifier.nvmap_handle;
		offset = args->condition.notifier.offset;

		handle_ref = nvhost_memmgr_get(memmgr, id, dev);
		if (IS_ERR(handle_ref)) {
			nvhost_err(d, "invalid notifier nvmap handle 0x%lx",
				   id);
			return -EINVAL;
		}

		notif = nvhost_memmgr_mmap(handle_ref);
		if (!notif) {
			nvhost_err(d, "failed to map notifier memory");
			return -ENOMEM;
		}

		notif = (struct notification *)((uintptr_t)notif + offset);

		/* user should set status pending before
		 * calling this ioctl */
		remain = wait_event_interruptible_timeout(
				ch->notifier_wq,
				notif->status == 0,
				timeout);

		if (remain == 0 && notif->status != 0) {
			ret = -ETIMEDOUT;
			goto notif_clean_up;
		} else if (remain < 0) {
			ret = -EINTR;
			goto notif_clean_up;
		}

		/* TBD: fill in correct information */
		jiffies = get_jiffies_64();
		jiffies_to_timespec(jiffies, &tv);
		notif->timestamp.nanoseconds[0] = tv.tv_nsec;
		notif->timestamp.nanoseconds[1] = tv.tv_sec;
		notif->info32 = 0xDEADBEEF; /* should be object name */
		notif->info16 = ch->hw_chid; /* should be method offset */

notif_clean_up:
		nvhost_memmgr_munmap(handle_ref, notif);
		return ret;
	case NVHOST_WAIT_TYPE_SEMAPHORE:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}


int gk20a_channel_zcull_bind(struct channel_gk20a *ch,
			    struct nvhost_zcull_bind_args *args)
{
	struct gk20a *g = ch->g;
	struct gr_gk20a *gr = &g->gr;

	nvhost_dbg_fn("");

	return gr_gk20a_bind_ctxsw_zcull(g, gr, ch,
				args->gpu_va, args->mode);
}

/* in this context the "channel" is the host1x channel which
 * maps to *all* gk20a channels */
int gk20a_channel_suspend(struct gk20a *g)
{
	struct fifo_gk20a *f = &g->fifo;
	u32 chid;
	bool channels_in_use = false;

	nvhost_dbg_fn("");

	for (chid = 0; chid < f->num_channels; chid++) {
		if (f->channel[chid].in_use) {

			nvhost_dbg_info("suspend channel %d", chid);

			/* disable channel */
			gk20a_writel(g, ccsr_channel_r(chid),
				gk20a_readl(g, ccsr_channel_r(chid)) |
				ccsr_channel_enable_clr_true_f());
			/* preempt the channel */
			gk20a_fifo_preempt_channel(g, chid);

			channels_in_use = true;
		}
	}

	if (channels_in_use) {
		gk20a_fifo_update_runlist(g, 0, ~0, false, true);

		for (chid = 0; chid < f->num_channels; chid++) {
			if (f->channel[chid].in_use)
				channel_gk20a_unbind(&f->channel[chid]);
		}
	}

	nvhost_dbg_fn("done");
	return 0;
}

/* in this context the "channel" is the host1x channel which
 * maps to *all* gk20a channels */
int gk20a_channel_resume(struct gk20a *g)
{
	struct fifo_gk20a *f = &g->fifo;
	u32 chid;
	bool channels_in_use = false;

	nvhost_dbg_fn("");

	for (chid = 0; chid < f->num_channels; chid++) {
		if (f->channel[chid].in_use) {
			nvhost_dbg_info("resume channel %d", chid);
			channel_gk20a_bind(&f->channel[chid]);
			channels_in_use = true;
		}
	}

	if (channels_in_use)
		gk20a_fifo_update_runlist(g, 0, ~0, true, true);

	nvhost_dbg_fn("done");
	return 0;
}
