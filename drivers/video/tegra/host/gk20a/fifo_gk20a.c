/*
 * drivers/video/tegra/host/gk20a/fifo_gk20a.c
 *
 * GK20A Graphics FIFO (gr host)
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
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <trace/events/nvhost.h>

#include "../dev.h"
#include "../nvhost_as.h"

#include "gk20a.h"
#include "hw_fifo_gk20a.h"
#include "hw_pbdma_gk20a.h"
#include "hw_ccsr_gk20a.h"
#include "hw_ram_gk20a.h"
#include "hw_proj_gk20a.h"
#include "hw_top_gk20a.h"
#include "hw_mc_gk20a.h"
#include "hw_gr_gk20a.h"

static int gk20a_fifo_update_runlist_locked(struct gk20a *g, u32 runlist_id,
					    u32 hw_chid, bool add,
					    bool wait_for_finish);
static void gk20a_fifo_handle_mmu_fault_thread(struct work_struct *work);

/*
 * Link engine IDs to MMU IDs and vice versa.
 */

static inline u32 gk20a_engine_id_to_mmu_id(u32 engine_id)
{
	switch (engine_id) {
	case ENGINE_GR_GK20A:
		return 0x00;
	case ENGINE_CE2_GK20A:
		return 0x1b;
	default:
		return ~0;
	}
}

static inline u32 gk20a_mmu_id_to_engine_id(u32 engine_id)
{
	switch (engine_id) {
	case 0x00:
		return ENGINE_GR_GK20A;
	case 0x1b:
		return ENGINE_CE2_GK20A;
	default:
		return ~0;
	}
}


static int init_engine_info(struct fifo_gk20a *f)
{
	struct fifo_engine_info_gk20a *gr_info;
	const u32 gr_sw_id = ENGINE_GR_GK20A;
	u32 i;
	u32 max_info_entries = top_device_info__size_1_v();

	nvhost_dbg_fn("");

	/* all we really care about finding is the graphics entry    */
	/* especially early on in sim it probably thinks it has more */
	f->num_engines = 1;

	gr_info = f->engine_info + gr_sw_id;

	gr_info->sw_id = gr_sw_id;
	gr_info->name = "gr";
	gr_info->dev_info_id = top_device_info_type_enum_graphics_v();
	gr_info->mmu_fault_id = fifo_intr_mmu_fault_eng_id_graphics_v();
	gr_info->runlist_id = ~0;
	gr_info->pbdma_id   = ~0;
	gr_info->engine_id  = ~0;

	for (i = 0; i < max_info_entries; i++) {
		u32 table_entry = gk20a_readl(f->g, top_device_info_r(i));
		u32 entry = top_device_info_entry_v(table_entry);
		u32 engine_enum = top_device_info_type_enum_v(table_entry);
		u32 table_entry2 = 0;

		if (entry == top_device_info_entry_not_valid_v())
			continue;

		if (top_device_info_chain_v(table_entry) ==
		    top_device_info_chain_enable_v()) {

			table_entry2 = gk20a_readl(f->g,
						   top_device_info_r(++i));

			engine_enum = top_device_info_type_enum_v(table_entry2);
		}

		/* we only care about GR engine here */
		if (entry == top_device_info_entry_enum_v() &&
		    engine_enum == gr_info->dev_info_id) {
			int pbdma_id;
			u32 runlist_bit;

			gr_info->runlist_id =
				top_device_info_runlist_enum_v(table_entry);
			nvhost_dbg_info("gr info: runlist_id %d", gr_info->runlist_id);

			gr_info->engine_id =
				top_device_info_engine_enum_v(table_entry);
			nvhost_dbg_info("gr info: engine_id %d", gr_info->engine_id);

			runlist_bit = 1 << gr_info->runlist_id;

			for (pbdma_id = 0; pbdma_id < f->num_pbdma; pbdma_id++) {
				nvhost_dbg_info("gr info: pbdma_map[%d]=%d",
					pbdma_id, f->pbdma_map[pbdma_id]);
				if (f->pbdma_map[pbdma_id] & runlist_bit)
					break;
			}

			if (pbdma_id == f->num_pbdma) {
				nvhost_dbg(dbg_err, "busted pbmda map");
				return -EINVAL;
			}
			gr_info->pbdma_id = pbdma_id;

			break;
		}
	}

	if (gr_info->runlist_id == ~0) {
		nvhost_dbg(dbg_err, "busted device info");
		return -EINVAL;
	}

	return 0;
}

void gk20a_remove_fifo_support(struct fifo_gk20a *f)
{
	struct gk20a *g = f->g;
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	struct fifo_engine_info_gk20a *engine_info;
	struct fifo_runlist_info_gk20a *runlist;
	u32 runlist_id;
	u32 i;

	nvhost_dbg_fn("");

	if (f->channel) {
		int c;
		for (c = 0; c < f->num_channels; c++) {
			if (f->channel[c].remove_support)
				f->channel[c].remove_support(f->channel+c);
		}
		kfree(f->channel);
	}

	g->mm.bar1.vm.unmap(&g->mm.bar1.vm, f->userd.gpu_va);

	nvhost_memmgr_munmap(f->userd.mem.ref, f->userd.cpu_va);
	nvhost_memmgr_free_sg_table(memmgr, f->userd.mem.ref, f->userd.mem.sgt);
	nvhost_memmgr_put(memmgr, f->userd.mem.ref);

	engine_info = f->engine_info + ENGINE_GR_GK20A;
	runlist_id = engine_info->runlist_id;
	runlist = &f->runlist_info[runlist_id];

	for (i = 0; i < MAX_RUNLIST_BUFFERS; i++) {
		nvhost_memmgr_free_sg_table(memmgr, runlist->mem[i].ref,
				runlist->mem[i].sgt);
		nvhost_memmgr_put(memmgr, runlist->mem[i].ref);
	}

	kfree(runlist->active_channels);

	kfree(f->runlist_info);
	kfree(f->pbdma_map);
	kfree(f->engine_info);
}

/* reads info from hardware and fills in pbmda exception info record */
static inline void get_exception_pbdma_info(
	struct gk20a *g,
	struct fifo_engine_info_gk20a *eng_info)
{
	struct fifo_pbdma_exception_info_gk20a *e =
		&eng_info->pbdma_exception_info;

	u32 pbdma_status_r = e->status_r = gk20a_readl(g,
		   fifo_pbdma_status_r(eng_info->pbdma_id));
	e->id = fifo_pbdma_status_id_v(pbdma_status_r); /* vs. id_hw_v()? */
	e->id_is_chid = fifo_pbdma_status_id_type_v(pbdma_status_r) ==
		fifo_pbdma_status_id_type_chid_v();
	e->chan_status_v  = fifo_pbdma_status_chan_status_v(pbdma_status_r);
	e->next_id_is_chid =
		fifo_pbdma_status_next_id_type_v(pbdma_status_r) ==
		fifo_pbdma_status_next_id_type_chid_v();
	e->next_id = fifo_pbdma_status_next_id_v(pbdma_status_r);
	e->chsw_in_progress =
		fifo_pbdma_status_chsw_v(pbdma_status_r) ==
		fifo_pbdma_status_chsw_in_progress_v();
}

static void fifo_pbdma_exception_status(struct gk20a *g,
	struct fifo_engine_info_gk20a *eng_info)
{
	struct fifo_pbdma_exception_info_gk20a *e;
	get_exception_pbdma_info(g, eng_info);
	e = &eng_info->pbdma_exception_info;

	nvhost_dbg_fn("pbdma_id %d, "
		      "id_type %s, id %d, chan_status %d, "
		      "next_id_type %s, next_id %d, "
		      "chsw_in_progress %d",
		      eng_info->pbdma_id,
		      e->id_is_chid ? "chid" : "tsgid", e->id, e->chan_status_v,
		      e->next_id_is_chid ? "chid" : "tsgid", e->next_id,
		      e->chsw_in_progress);
}

/* reads info from hardware and fills in pbmda exception info record */
static inline void get_exception_engine_info(
	struct gk20a *g,
	struct fifo_engine_info_gk20a *eng_info)
{
	struct fifo_engine_exception_info_gk20a *e =
		&eng_info->engine_exception_info;
	u32 engine_status_r = e->status_r =
		gk20a_readl(g, fifo_engine_status_r(eng_info->engine_id));
	e->id = fifo_engine_status_id_v(engine_status_r); /* vs. id_hw_v()? */
	e->id_is_chid = fifo_engine_status_id_type_v(engine_status_r) ==
		fifo_engine_status_id_type_chid_v();
	e->ctx_status_v = fifo_engine_status_ctx_status_v(engine_status_r);
	e->faulted =
		fifo_engine_status_faulted_v(engine_status_r) ==
		fifo_engine_status_faulted_true_v();
	e->idle =
		fifo_engine_status_engine_v(engine_status_r) ==
		fifo_engine_status_engine_idle_v();
	e->ctxsw_in_progress =
		fifo_engine_status_ctxsw_v(engine_status_r) ==
		fifo_engine_status_ctxsw_in_progress_v();
}

static void fifo_engine_exception_status(struct gk20a *g,
			       struct fifo_engine_info_gk20a *eng_info)
{
	struct fifo_engine_exception_info_gk20a *e;
	get_exception_engine_info(g, eng_info);
	e = &eng_info->engine_exception_info;

	nvhost_dbg_fn("engine_id %d, id_type %s, id %d, ctx_status %d, "
		      "faulted %d, idle %d, ctxsw_in_progress %d, ",
		      eng_info->engine_id, e->id_is_chid ? "chid" : "tsgid",
		      e->id, e->ctx_status_v,
		      e->faulted, e->idle,  e->ctxsw_in_progress);
}

static int init_runlist(struct gk20a *g, struct fifo_gk20a *f)
{
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	struct fifo_engine_info_gk20a *engine_info;
	struct fifo_runlist_info_gk20a *runlist;
	u32 runlist_id;
	u32 i;
	u64 runlist_size;

	nvhost_dbg_fn("");

	f->max_runlists = fifo_eng_runlist_base__size_1_v();
	f->runlist_info = kzalloc(sizeof(struct fifo_runlist_info_gk20a) *
				  f->max_runlists, GFP_KERNEL);
	if (!f->runlist_info)
		goto clean_up;

	engine_info = f->engine_info + ENGINE_GR_GK20A;
	runlist_id = engine_info->runlist_id;
	runlist = &f->runlist_info[runlist_id];

	runlist->active_channels =
		kzalloc(DIV_ROUND_UP(f->num_channels, BITS_PER_BYTE),
			GFP_KERNEL);
	if (!runlist->active_channels)
		goto clean_up_runlist_info;

	runlist_size  = ram_rl_entry_size_v() * f->num_channels;
	for (i = 0; i < MAX_RUNLIST_BUFFERS; i++) {
		struct sg_table *sgt;
		runlist->mem[i].ref =
			nvhost_memmgr_alloc(memmgr, runlist_size,
					    DEFAULT_ALLOC_ALIGNMENT,
					    DEFAULT_ALLOC_FLAGS,
					    0);
		if (IS_ERR(runlist->mem[i].ref))
			goto clean_up_runlist;
		sgt = nvhost_memmgr_sg_table(memmgr, runlist->mem[i].ref);
		if (IS_ERR(sgt))
			goto clean_up_runlist;
		runlist->mem[i].sgt = sgt;
		runlist->mem[i].size = runlist_size;
	}
	mutex_init(&runlist->mutex);
	init_waitqueue_head(&runlist->runlist_wq);

	/* None of buffers is pinned if this value doesn't change.
	    Otherwise, one of them (cur_buffer) must have been pinned. */
	runlist->cur_buffer = MAX_RUNLIST_BUFFERS;

	nvhost_dbg_fn("done");
	return 0;

clean_up_runlist:
	for (i = 0; i < MAX_RUNLIST_BUFFERS; i++) {
		if (runlist->mem[i].sgt)
			nvhost_memmgr_free_sg_table(memmgr, runlist->mem[i].ref,
					runlist->mem[i].sgt);
		if (runlist->mem[i].ref)
			nvhost_memmgr_put(memmgr, runlist->mem[i].ref);
	}

	kfree(runlist->active_channels);
	runlist->active_channels = NULL;

clean_up_runlist_info:
	kfree(f->runlist_info);
	f->runlist_info = NULL;

clean_up:
	nvhost_dbg_fn("fail");
	return -ENOMEM;
}

static int gk20a_init_fifo_reset_enable_hw(struct gk20a *g)
{
	u32 pmc_enable;
	u32 intr_stall;
	u32 mask;
	u32 timeout;
	int i;

	nvhost_dbg_fn("");

	/* enable pmc pfifo */
	pmc_enable = gk20a_readl(g, mc_enable_r());
	pmc_enable &= ~mc_enable_pfifo_enabled_f();
	pmc_enable &= ~mc_enable_ce2_enabled_f();
	gk20a_writel(g, mc_enable_r(), pmc_enable);

	pmc_enable = gk20a_readl(g, mc_enable_r());
	pmc_enable |= mc_enable_pfifo_enabled_f();
	pmc_enable |= mc_enable_ce2_enabled_f();
	gk20a_writel(g, mc_enable_r(), pmc_enable);
	gk20a_readl(g, mc_enable_r());

	/* enable pbdma */
	mask = 0;
	for (i = 0; i < proj_host_num_pbdma_v(); ++i)
		mask |= mc_enable_pb_sel_f(mc_enable_pb_0_enabled_v(), i);
	gk20a_writel(g, mc_enable_pb_r(), mask);

	/* enable pfifo interrupt */
	gk20a_writel(g, fifo_intr_0_r(), 0xFFFFFFFF);
	gk20a_writel(g, fifo_intr_en_0_r(), 0xFFFFFFFF); /* TBD: alternative intr tree*/
	gk20a_writel(g, fifo_intr_en_1_r(), 0xFFFFFFFF); /* TBD: alternative intr tree*/

	/* enable pbdma interrupt */
	mask = 0;
	for (i = 0; i < proj_host_num_pbdma_v(); i++) {
		intr_stall = gk20a_readl(g, pbdma_intr_stall_r(i));
		intr_stall &= ~pbdma_intr_stall_lbreq_enabled_f();
		gk20a_writel(g, pbdma_intr_stall_r(i), intr_stall);
		gk20a_writel(g, pbdma_intr_0_r(i), 0xFFFFFFFF);
		gk20a_writel(g, pbdma_intr_en_0_r(i),
			(~0) & ~pbdma_intr_en_0_lbreq_enabled_f());
		gk20a_writel(g, pbdma_intr_1_r(i), 0xFFFFFFFF);
		gk20a_writel(g, pbdma_intr_en_1_r(i), 0xFFFFFFFF);
	}

	/* TBD: apply overrides */

	/* TBD: BLCG prod */

	/* reset runlist interrupts */
	gk20a_writel(g, fifo_intr_runlist_r(), ~0);

	/* TBD: do we need those? */
	timeout = gk20a_readl(g, fifo_fb_timeout_r());
	timeout = set_field(timeout, fifo_fb_timeout_period_m(),
			fifo_fb_timeout_period_max_f());
	gk20a_writel(g, fifo_fb_timeout_r(), timeout);

	timeout = gk20a_readl(g, fifo_pb_timeout_r());
	timeout &= ~fifo_pb_timeout_detection_enabled_f();
	gk20a_writel(g, fifo_pb_timeout_r(), timeout);

	nvhost_dbg_fn("done");

	return 0;
}

static void gk20a_init_fifo_pbdma_intr_descs(struct fifo_gk20a *f)
{
	/* These are all errors which indicate something really wrong
	 * going on in the device. */
	f->intr.pbdma.device_fatal_0 =
		pbdma_intr_0_memreq_pending_f() |
		pbdma_intr_0_memack_timeout_pending_f() |
		pbdma_intr_0_memack_extra_pending_f() |
		pbdma_intr_0_memdat_timeout_pending_f() |
		pbdma_intr_0_memdat_extra_pending_f() |
		pbdma_intr_0_memflush_pending_f() |
		pbdma_intr_0_memop_pending_f() |
		pbdma_intr_0_lbconnect_pending_f() |
		pbdma_intr_0_lbreq_pending_f() |
		pbdma_intr_0_lback_timeout_pending_f() |
		pbdma_intr_0_lback_extra_pending_f() |
		pbdma_intr_0_lbdat_timeout_pending_f() |
		pbdma_intr_0_lbdat_extra_pending_f() |
		pbdma_intr_0_xbarconnect_pending_f() |
		pbdma_intr_0_pri_pending_f();

	/* These are data parsing, framing errors or others which can be
	 * recovered from with intervention... or just resetting the
	 * channel. */
	f->intr.pbdma.channel_fatal_0 =
		pbdma_intr_0_gpfifo_pending_f() |
		pbdma_intr_0_gpptr_pending_f() |
		pbdma_intr_0_gpentry_pending_f() |
		pbdma_intr_0_gpcrc_pending_f() |
		pbdma_intr_0_pbptr_pending_f() |
		pbdma_intr_0_pbentry_pending_f() |
		pbdma_intr_0_pbcrc_pending_f() |
		pbdma_intr_0_method_pending_f() |
		pbdma_intr_0_methodcrc_pending_f() |
		pbdma_intr_0_semaphore_pending_f() |
		pbdma_intr_0_pbseg_pending_f() |
		pbdma_intr_0_signature_pending_f();

	/* Can be used for sw-methods, or represents
	 * a recoverable timeout. */
	f->intr.pbdma.restartable_0 =
		pbdma_intr_0_device_pending_f() |
		pbdma_intr_0_acquire_pending_f();
}

static int gk20a_init_fifo_setup_sw(struct gk20a *g)
{
	struct mem_mgr *memmgr = mem_mgr_from_g(g);
	struct fifo_gk20a *f = &g->fifo;
	int chid, i, err;

	nvhost_dbg_fn("");

	if (f->sw_ready) {
		nvhost_dbg_fn("skip init");
		return 0;
	}

	f->g = g;

	INIT_WORK(&f->fault_restore_thread,
		  gk20a_fifo_handle_mmu_fault_thread);
	mutex_init(&f->intr.isr.mutex);
	gk20a_init_fifo_pbdma_intr_descs(f); /* just filling in data/tables */

	f->num_channels = ccsr_channel__size_1_v();
	f->num_pbdma = proj_host_num_pbdma_v();
	f->max_engines = ENGINE_INVAL_GK20A;

	f->userd_entry_size = 1 << ram_userd_base_shift_v();
	f->userd_total_size = f->userd_entry_size * f->num_channels;

	f->userd.mem.ref = nvhost_memmgr_alloc(memmgr, f->userd_total_size,
					       4096, /* 4K pages */
					       DEFAULT_ALLOC_FLAGS,
					       0);
	if (IS_ERR(f->userd.mem.ref)) {
		err = PTR_ERR(f->userd.mem.ref);
		goto clean_up;
	}

	f->userd.cpu_va = nvhost_memmgr_mmap(f->userd.mem.ref);
	/* f->userd.cpu_va = g->bar1; */
	if (!f->userd.cpu_va) {
		f->userd.cpu_va = NULL;
		err = -ENOMEM;
		goto clean_up;
	}

	/* bar1 va */
	f->userd.gpu_va = g->mm.bar1.vm.map(&g->mm.bar1.vm,
					    memmgr,
					    f->userd.mem.ref,
					    /*offset_align, flags, kind*/
					    4096, 0, 0,
					    &f->userd.mem.sgt,
					    false,
					    mem_flag_none);
	f->userd.cpu_pa = gk20a_mm_iova_addr(f->userd.mem.sgt->sgl);
	nvhost_dbg(dbg_map, "userd physical address : 0x%08llx - 0x%08llx",
			f->userd.cpu_pa, f->userd.cpu_pa + f->userd_total_size);
	nvhost_dbg(dbg_map, "userd bar1 va = 0x%llx", f->userd.gpu_va);

	f->userd.mem.size = f->userd_total_size;

	f->channel = kzalloc(f->num_channels * sizeof(*f->channel),
				GFP_KERNEL);
	f->pbdma_map = kzalloc(f->num_pbdma * sizeof(*f->pbdma_map),
				GFP_KERNEL);
	f->engine_info = kzalloc(f->max_engines * sizeof(*f->engine_info),
				GFP_KERNEL);

	if (!(f->channel && f->pbdma_map && f->engine_info)) {
		err = -ENOMEM;
		goto clean_up;
	}

	/* pbdma map needs to be in place before calling engine info init */
	for (i = 0; i < f->num_pbdma; ++i)
		f->pbdma_map[i] = gk20a_readl(g, fifo_pbdma_map_r(i));

	init_engine_info(f);

	init_runlist(g, f);

	for (chid = 0; chid < f->num_channels; chid++) {
		f->channel[chid].userd_cpu_va =
			f->userd.cpu_va + chid * f->userd_entry_size;
		f->channel[chid].userd_cpu_pa =
			f->userd.cpu_pa + chid * f->userd_entry_size;
		f->channel[chid].userd_gpu_va =
			f->userd.gpu_va + chid * f->userd_entry_size;

		gk20a_init_channel_support(g, chid);
	}
	mutex_init(&f->ch_inuse_mutex);

	f->remove_support = gk20a_remove_fifo_support;
	f->sw_ready = true;

	nvhost_dbg_fn("done");
	return 0;

clean_up:
	nvhost_dbg_fn("fail");
	nvhost_memmgr_munmap(f->userd.mem.ref, f->userd.cpu_va);
	if (f->userd.gpu_va)
		g->mm.bar1.vm.unmap(&g->mm.bar1.vm, f->userd.gpu_va);
	nvhost_memmgr_put(memmgr, f->userd.mem.ref);
	memset(&f->userd, 0, sizeof(struct userd_desc));

	kfree(f->channel);
	f->channel = NULL;
	kfree(f->pbdma_map);
	f->pbdma_map = NULL;
	kfree(f->engine_info);
	f->engine_info = NULL;

	return err;
}

static void gk20a_fifo_handle_runlist_event(struct gk20a *g)
{
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_runlist_info_gk20a *runlist;
	unsigned long runlist_event;
	u32 runlist_id;

	runlist_event = gk20a_readl(g, fifo_intr_runlist_r());
	gk20a_writel(g, fifo_intr_runlist_r(), runlist_event);

	for_each_set_bit(runlist_id, &runlist_event, f->max_runlists) {
		runlist = &f->runlist_info[runlist_id];
		wake_up(&runlist->runlist_wq);
	}
}

static int gk20a_init_fifo_setup_hw(struct gk20a *g)
{
	struct fifo_gk20a *f = &g->fifo;

	nvhost_dbg_fn("");

	/* test write, read through bar1 @ userd region before
	 * turning on the snooping */
	{
		struct fifo_gk20a *f = &g->fifo;
		u32 v, v1 = 0x33, v2 = 0x55;

		u32 bar1_vaddr = f->userd.gpu_va;
		volatile u32 *cpu_vaddr = f->userd.cpu_va;

		nvhost_dbg_info("test bar1 @ vaddr 0x%x",
			   bar1_vaddr);

		v = gk20a_bar1_readl(g, bar1_vaddr);

		*cpu_vaddr = v1;
		smp_mb();

		if (v1 != gk20a_bar1_readl(g, bar1_vaddr)) {
			nvhost_err(dev_from_gk20a(g), "bar1 broken @ gk20a!");
			return -EINVAL;
		}

		gk20a_bar1_writel(g, bar1_vaddr, v2);

		if (v2 != gk20a_bar1_readl(g, bar1_vaddr)) {
			nvhost_err(dev_from_gk20a(g), "bar1 broken @ gk20a!");
			return -EINVAL;
		}

		/* is it visible to the cpu? */
		if (*cpu_vaddr != v2) {
			nvhost_err(dev_from_gk20a(g),
				"cpu didn't see bar1 write @ %p!",
				cpu_vaddr);
		}

		/* put it back */
		gk20a_bar1_writel(g, bar1_vaddr, v);
	}

	/*XXX all manner of flushes and caching worries, etc */

	/* set the base for the userd region now */
	gk20a_writel(g, fifo_bar1_base_r(),
			fifo_bar1_base_ptr_f(f->userd.gpu_va >> 12) |
			fifo_bar1_base_valid_true_f());

	nvhost_dbg_fn("done");

	return 0;
}

int gk20a_init_fifo_support(struct gk20a *g)
{
	u32 err;

	err = gk20a_init_fifo_reset_enable_hw(g);
	if (err)
		return err;

	err = gk20a_init_fifo_setup_sw(g);
	if (err)
		return err;

	err = gk20a_init_fifo_setup_hw(g);
	if (err)
		return err;

	return err;
}

static struct channel_gk20a *
channel_from_inst_ptr(struct fifo_gk20a *f, u64 inst_ptr)
{
	int ci;
	if (unlikely(!f->channel))
		return NULL;
	for (ci = 0; ci < f->num_channels; ci++) {
		struct channel_gk20a *c = f->channel+ci;
		if (c->inst_block.mem.ref &&
		    (inst_ptr == (u64)(sg_phys(c->inst_block.mem.sgt->sgl))))
			return f->channel+ci;
	}
	return NULL;
}

/* fault info/descriptions.
 * tbd: move to setup
 *  */
static const char * const fault_type_descs[] = {
	 "pde", /*fifo_intr_mmu_fault_info_type_pde_v() == 0 */
	 "pde size",
	 "pte",
	 "va limit viol",
	 "unbound inst",
	 "priv viol",
	 "ro viol",
	 "wo viol",
	 "pitch mask",
	 "work creation",
	 "bad aperture",
	 "compression failure",
	 "bad kind",
	 "region viol",
	 "dual ptes",
	 "poisoned",
};
/* engine descriptions */
static const char * const engine_subid_descs[] = {
	"gpc",
	"hub",
};

static const char * const hub_client_descs[] = {
	"vip", "ce0", "ce1", "dniso", "fe", "fecs", "host", "host cpu",
	"host cpu nb", "iso", "mmu", "mspdec", "msppp", "msvld",
	"niso", "p2p", "pd", "perf", "pmu", "raster twod", "scc",
	"scc nb", "sec", "ssync", "gr copy", "ce2", "xv", "mmu nb",
	"msenc", "d falcon", "sked", "a falcon", "n/a",
};

static const char * const gpc_client_descs[] = {
	"l1 0", "t1 0", "pe 0",
	"l1 1", "t1 1", "pe 1",
	"l1 2", "t1 2", "pe 2",
	"l1 3", "t1 3", "pe 3",
	"rast", "gcc", "gpccs",
	"prop 0", "prop 1", "prop 2", "prop 3",
	"l1 4", "t1 4", "pe 4",
	"l1 5", "t1 5", "pe 5",
	"l1 6", "t1 6", "pe 6",
	"l1 7", "t1 7", "pe 7",
	"gpm",
	"ltp utlb 0", "ltp utlb 1", "ltp utlb 2", "ltp utlb 3",
	"rgg utlb",
};

/* reads info from hardware and fills in mmu fault info record */
static inline void get_exception_mmu_fault_info(
	struct gk20a *g, u32 engine_id,
	struct fifo_mmu_fault_info_gk20a *f)
{
	u32 fault_info_v;

	nvhost_dbg_fn("engine_id %d", engine_id);

	memset(f, 0, sizeof(*f));

	f->fault_info_v = fault_info_v = gk20a_readl(g,
	     fifo_intr_mmu_fault_info_r(engine_id));
	f->fault_type_v =
		fifo_intr_mmu_fault_info_type_v(fault_info_v);
	f->engine_subid_v =
		fifo_intr_mmu_fault_info_engine_subid_v(fault_info_v);
	f->client_v = fifo_intr_mmu_fault_info_client_v(fault_info_v);

	BUG_ON(f->fault_type_v >= ARRAY_SIZE(fault_type_descs));
	f->fault_type_desc =  fault_type_descs[f->fault_type_v];

	BUG_ON(f->engine_subid_v >= ARRAY_SIZE(engine_subid_descs));
	f->engine_subid_desc = engine_subid_descs[f->engine_subid_v];

	if (f->engine_subid_v ==
	    fifo_intr_mmu_fault_info_engine_subid_hub_v()) {

		BUG_ON(f->client_v >= ARRAY_SIZE(hub_client_descs));
		f->client_desc = hub_client_descs[f->client_v];
	} else if (f->engine_subid_v ==
		   fifo_intr_mmu_fault_info_engine_subid_gpc_v()) {
		BUG_ON(f->client_v >= ARRAY_SIZE(gpc_client_descs));
		f->client_desc = gpc_client_descs[f->client_v];
	} else {
		BUG_ON(1);
	}

	f->fault_hi_v = gk20a_readl(g, fifo_intr_mmu_fault_hi_r(engine_id));
	f->fault_lo_v = gk20a_readl(g, fifo_intr_mmu_fault_lo_r(engine_id));
	/* note:ignoring aperture on gk20a... */
	f->inst_ptr = fifo_intr_mmu_fault_inst_ptr_v(
		 gk20a_readl(g, fifo_intr_mmu_fault_inst_r(engine_id)));
	/* note: inst_ptr is a 40b phys addr.  */
	f->inst_ptr <<= fifo_intr_mmu_fault_inst_ptr_align_shift_v();
}

static void gk20a_fifo_reset_engine(struct gk20a *g, u32 engine_id)
{
	u32 pmc_enable = gk20a_readl(g, mc_enable_r());
	u32 pmc_enable_reset = pmc_enable;

	nvhost_dbg_fn("");

	if (engine_id == top_device_info_type_enum_graphics_v()) {
		/* resetting engine using mc_enable_r() is not enough,
		 * we do full init sequence */
		gk20a_gr_reset(g);
	}
	if (engine_id == top_device_info_type_enum_copy0_v()) {
		pmc_enable_reset &= ~mc_enable_ce2_m();

		nvhost_dbg(dbg_intr, "PMC before: %08x reset: %08x\n",
				pmc_enable, pmc_enable_reset);
		gk20a_writel(g, mc_enable_r(), pmc_enable_reset);
		usleep_range(1000, 2000);
		gk20a_writel(g, mc_enable_r(), pmc_enable);
		gk20a_readl(g, mc_enable_r());
	}
}

static void gk20a_fifo_handle_mmu_fault_thread(struct work_struct *work)
{
	struct fifo_gk20a *f = container_of(work, struct fifo_gk20a,
					    fault_restore_thread);
	struct gk20a *g = f->g;
	int i;

	gk20a_init_pmu_support(g);

	/* Restore the runlist */
	for (i = 0; i < g->fifo.max_runlists; i++)
		gk20a_fifo_update_runlist_locked(g, i, ~0, true, true);

	/* unlock all runlists */
	for (i = 0; i < g->fifo.max_runlists; i++)
		mutex_unlock(&g->fifo.runlist_info[i].mutex);

}

static void gk20a_fifo_handle_chsw_fault(struct gk20a *g)
{
	u32 intr;

	intr = gk20a_readl(g, fifo_intr_chsw_error_r());
	nvhost_err(dev_from_gk20a(g), "chsw: %08x\n", intr);
	gk20a_writel(g, fifo_intr_chsw_error_r(), intr);
}

static void gk20a_fifo_handle_mmu_fault(struct gk20a *g)
{
	bool fake_fault;
	ulong fault_id;
	u32 engine_mmu_id;
	int i;

	nvhost_dbg_fn("");

	nvhost_debug_dump(g->host);

	/* PMU does not survive GR reset. */
	gk20a_pmu_destroy(g);

	/* If we have recovery in progress, MMU fault id is invalid */
	if (g->fifo.mmu_fault_engines) {
		fault_id = g->fifo.mmu_fault_engines;
		g->fifo.mmu_fault_engines = 0;
		fake_fault = true;
	} else {
		fault_id = gk20a_readl(g, fifo_intr_mmu_fault_id_r());
		fake_fault = false;
	}

	/* lock all runlists. Note that locks are are released in
	 * gk20a_fifo_handle_mmu_fault_thread() */
	for (i = 0; i < g->fifo.max_runlists; i++)
		mutex_lock(&g->fifo.runlist_info[i].mutex);

	/* go through all faulted engines */
	for_each_set_bit(engine_mmu_id, &fault_id, BITS_PER_LONG) {
		/* bits in fifo_intr_mmu_fault_id_r do not correspond 1:1 to
		 * engines. Convert engine_mmu_id to engine_id */
		u32 engine_id = gk20a_mmu_id_to_engine_id(engine_mmu_id);
		struct fifo_runlist_info_gk20a *runlist = g->fifo.runlist_info;
		struct fifo_mmu_fault_info_gk20a f;
		struct channel_gk20a *ch = NULL;

		get_exception_mmu_fault_info(g, engine_mmu_id, &f);
		trace_nvhost_gk20a_mmu_fault(f.fault_hi_v,
					     f.fault_lo_v,
					     f.fault_info_v,
					     f.inst_ptr,
					     engine_id,
					     f.engine_subid_desc,
					     f.client_desc,
					     f.fault_type_desc);
		nvhost_err(dev_from_gk20a(g), "mmu fault on engine %d, "
			   "engined subid %d (%s), client %d (%s), "
			   "addr 0x%08x:0x%08x, type %d (%s), info 0x%08x,"
			   "inst_ptr 0x%llx\n",
			   engine_id,
			   f.engine_subid_v, f.engine_subid_desc,
			   f.client_v, f.client_desc,
			   f.fault_hi_v, f.fault_lo_v,
			   f.fault_type_v, f.fault_type_desc,
			   f.fault_info_v, f.inst_ptr);

		/* get the channel */
		if (fake_fault) {
			/* read and parse engine status */
			u32 status = gk20a_readl(g,
				fifo_engine_status_r(engine_id));
			u32 ctx_status =
				fifo_engine_status_ctx_status_v(status);
			bool type_ch = fifo_pbdma_status_id_type_v(status) ==
				fifo_pbdma_status_id_type_chid_v();

			/* use next_id if context load is failing */
			u32 id = (ctx_status ==
				fifo_engine_status_ctx_status_ctxsw_load_v()) ?
				fifo_engine_status_next_id_v(status) :
				fifo_engine_status_id_v(status);

			if (type_ch) {
				ch = g->fifo.channel + id;
			} else {
				nvhost_err(dev_from_gk20a(g), "non-chid type not supported");
				WARN_ON(1);
			}
		} else {
			/* read channel based on instruction pointer */
			ch = channel_from_inst_ptr(&g->fifo, f.inst_ptr);
		}

		if (ch) {
			if (ch->hwctx) {
				nvhost_err(dev_from_gk20a(g), "channel with hwctx has generated an mmu fault");
				ch->hwctx->has_timedout = true;
			}

			if (ch->in_use) {

				/* disable the channel from hw and increment
				 * syncpoints */
				gk20a_disable_channel_no_update(ch);

				/* remove the channel from runlist */
				clear_bit(ch->hw_chid,
					  runlist->active_channels);

				/* mark channel as faulted */
				ch->hwctx->has_timedout = true;
			}
		} else if (f.inst_ptr ==
				sg_phys(g->mm.bar1.inst_block.mem.sgt->sgl)) {
			nvhost_err(dev_from_gk20a(g), "mmu fault from bar1");
		} else if (f.inst_ptr ==
				sg_phys(g->mm.pmu.inst_block.mem.sgt->sgl)) {
			nvhost_err(dev_from_gk20a(g), "mmu fault from pmu");
		} else
			nvhost_err(dev_from_gk20a(g), "couldn't locate channel for mmu fault");
	}

	/* reset engines */
	for_each_set_bit(engine_mmu_id, &fault_id, BITS_PER_LONG) {
		u32 engine_id = gk20a_mmu_id_to_engine_id(engine_mmu_id);
		if (engine_id != ~0)
			gk20a_fifo_reset_engine(g, engine_id);
	}

	/* CLEAR the runlists. Do not wait for runlist to start as
	 * some engines may not be available right now */
	for (i = 0; i < g->fifo.max_runlists; i++)
		gk20a_fifo_update_runlist_locked(g, i, ~0, false, false);

	/* clear interrupt */
	gk20a_writel(g, fifo_intr_mmu_fault_id_r(), fault_id);

	/* resume scheduler */
	gk20a_writel(g, fifo_error_sched_disable_r(),
		     gk20a_readl(g, fifo_error_sched_disable_r()));

	/* Spawn a work to enable PMU and restore runlists */
	schedule_work(&g->fifo.fault_restore_thread);
}

void gk20a_fifo_recover(struct gk20a *g, ulong engine_ids)
{
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	unsigned long delay = GR_IDLE_CHECK_DEFAULT;
	u32 engine_id;
	int ret;

	/* store faulted engines in advance */
	g->fifo.mmu_fault_engines = 0;
	for_each_set_bit(engine_id, &engine_ids, BITS_PER_LONG)
		g->fifo.mmu_fault_engines |=
			BIT(gk20a_engine_id_to_mmu_id(engine_id));

	/* trigger faults for all bad engines */
	for_each_set_bit(engine_id, &engine_ids, BITS_PER_LONG) {
		if (engine_id > g->fifo.max_engines) {
			WARN_ON(true);
			break;
		}

		gk20a_writel(g, fifo_trigger_mmu_fault_r(engine_id),
			     fifo_trigger_mmu_fault_id_f(
			     gk20a_engine_id_to_mmu_id(engine_id)) |
			     fifo_trigger_mmu_fault_enable_f(1));
	}

	/* Wait for MMU fault to trigger */
	ret = -EBUSY;
	do {
		if (gk20a_readl(g, fifo_intr_0_r()) &
				fifo_intr_0_mmu_fault_pending_f()) {
			ret = 0;
			break;
		}

		usleep_range(delay, delay * 2);
		delay = min_t(u32, delay << 1, GR_IDLE_CHECK_MAX);
	} while (time_before(jiffies, end_jiffies) |
			!tegra_platform_is_silicon());

	if (ret)
		nvhost_err(dev_from_gk20a(g), "mmu fault timeout");

	/* release mmu fault trigger */
	for_each_set_bit(engine_id, &engine_ids, BITS_PER_LONG)
		gk20a_writel(g, fifo_trigger_mmu_fault_r(engine_id), 0);
}

static bool gk20a_fifo_handle_sched_error(struct gk20a *g)
{
	u32 sched_error;
	u32 engine_id;
	int id = -1;
	bool non_chid = false;

	/* read and reset the scheduler error register */
	sched_error = gk20a_readl(g, fifo_intr_sched_error_r());
	gk20a_writel(g, fifo_intr_0_r(), fifo_intr_0_sched_error_reset_f());

	for (engine_id = 0; engine_id < g->fifo.max_engines; engine_id++) {
		u32 status = gk20a_readl(g, fifo_engine_status_r(engine_id));
		u32 ctx_status = fifo_engine_status_ctx_status_v(status);
		bool failing_engine;

		/* we are interested in busy engines */
		failing_engine = fifo_engine_status_engine_v(status) ==
			fifo_engine_status_engine_busy_v();

		/* ..that are doing context switch */
		failing_engine = failing_engine &&
			(ctx_status ==
				fifo_engine_status_ctx_status_ctxsw_switch_v()
			|| ctx_status ==
				fifo_engine_status_ctx_status_ctxsw_save_v()
			|| ctx_status ==
				fifo_engine_status_ctx_status_ctxsw_load_v());

		if (failing_engine) {
			id = (ctx_status ==
				fifo_engine_status_ctx_status_ctxsw_load_v()) ?
				fifo_engine_status_next_id_v(status) :
				fifo_engine_status_id_v(status);
			non_chid = fifo_pbdma_status_id_type_v(status) !=
				fifo_pbdma_status_id_type_chid_v();
			break;
		}
	}

	nvhost_err(dev_from_gk20a(g), "fifo sched error : 0x%08x, engine=%u, %s=%d",
		   sched_error, engine_id, non_chid ? "non-ch" : "ch", id);

	/* could not find the engine - should never happen */
	if (unlikely(engine_id >= g->fifo.max_engines))
		return true;

	if (fifo_intr_sched_error_code_f(sched_error) ==
	    fifo_intr_sched_error_code_ctxsw_timeout_v()) {
		gk20a_fifo_recover(g, BIT(engine_id));
		return false;
	}

	return true;
}

static u32 fifo_error_isr(struct gk20a *g, u32 fifo_intr)
{
	bool reset_channel = false, reset_engine = false;
	struct device *dev = dev_from_gk20a(g);
	u32 handled = 0;

	nvhost_dbg_fn("");

	if (fifo_intr & fifo_intr_0_pio_error_pending_f()) {
		/* pio mode is unused.  this shouldn't happen, ever. */
		/* should we clear it or just leave it pending? */
		nvhost_err(dev, "fifo pio error!\n");
		BUG_ON(1);
	}

	if (fifo_intr & fifo_intr_0_bind_error_pending_f()) {
		u32 bind_error = gk20a_readl(g, fifo_intr_bind_error_r());
		nvhost_err(dev, "fifo bind error: 0x%08x", bind_error);
		reset_channel = true;
		handled |= fifo_intr_0_bind_error_pending_f();
	}

	if (fifo_intr & fifo_intr_0_sched_error_pending_f()) {
		reset_channel = gk20a_fifo_handle_sched_error(g);
		handled |= fifo_intr_0_sched_error_pending_f();
	}

	if (fifo_intr & fifo_intr_0_chsw_error_pending_f()) {
		gk20a_fifo_handle_chsw_fault(g);
		handled |= fifo_intr_0_chsw_error_pending_f();
	}

	if (fifo_intr & fifo_intr_0_mmu_fault_pending_f()) {
		gk20a_fifo_handle_mmu_fault(g);
		reset_channel = true;
		reset_engine  = true;
		handled |= fifo_intr_0_mmu_fault_pending_f();
	}

	reset_channel = reset_channel || fifo_intr;

	if (reset_channel) {
		int engine_id;
		nvhost_err(dev_from_gk20a(g),
			   "channel reset initated from %s", __func__);
		for (engine_id = 0;
		     engine_id < g->fifo.max_engines;
		     engine_id++) {
			nvhost_dbg_fn("enum:%d -> engine_id:%d", engine_id,
				      g->fifo.engine_info[engine_id].engine_id);
			fifo_pbdma_exception_status(g,
					&g->fifo.engine_info[engine_id]);
			fifo_engine_exception_status(g,
					&g->fifo.engine_info[engine_id]);
		}
	}

	return handled;
}


static u32 gk20a_fifo_handle_pbdma_intr(struct device *dev,
					struct gk20a *g,
					struct fifo_gk20a *f,
					u32 pbdma_id)
{
	u32 pbdma_intr_0 = gk20a_readl(g, pbdma_intr_0_r(pbdma_id));
	u32 pbdma_intr_1 = gk20a_readl(g, pbdma_intr_1_r(pbdma_id));
	u32 handled = 0;
	bool reset_device = false;
	bool reset_channel = false;

	nvhost_dbg_fn("");

	if (pbdma_intr_0) {
		if (f->intr.pbdma.device_fatal_0 & pbdma_intr_0) {
			dev_err(dev, "unrecoverable device error: "
				"pbdma_intr_0(%d):0x%08x", pbdma_id, pbdma_intr_0);
			reset_device = true;
			/* TODO: disable pbdma intrs */
			handled |= f->intr.pbdma.device_fatal_0 & pbdma_intr_0;
		}
		if (f->intr.pbdma.channel_fatal_0 & pbdma_intr_0) {
			dev_warn(dev, "channel error: "
				 "pbdma_intr_0(%d):0x%08x", pbdma_id, pbdma_intr_0);
			reset_channel = true;
			/* TODO: clear pbdma channel errors */
			handled |= f->intr.pbdma.channel_fatal_0 & pbdma_intr_0;
		}
		gk20a_writel(g, pbdma_intr_0_r(pbdma_id), pbdma_intr_0);
	}

	/* all intrs in _intr_1 are "host copy engine" related,
	 * which gk20a doesn't have. for now just make them channel fatal. */
	if (pbdma_intr_1) {
		dev_err(dev, "channel hce error: pbdma_intr_1(%d): 0x%08x",
			pbdma_id, pbdma_intr_1);
		reset_channel = true;
		gk20a_writel(g, pbdma_intr_1_r(pbdma_id), pbdma_intr_1);
	}



	return handled;
}

static u32 fifo_channel_isr(struct gk20a *g, u32 fifo_intr)
{
	struct device *dev = dev_from_gk20a(g);
	/* Note: we don't have any of these in use (yet) for gk20a.
	 * These are usually if not always coming from non-stall,
	 * notification type interrupts.  It isn't necessarily
	 * anything to do with the channel currently running.
	 * Clear it and warn...
	 */
	dev_warn(dev, "unexpected channel (non-stall?) interrupt");
	return fifo_intr_0_channel_intr_pending_f();
}


static u32 fifo_pbdma_isr(struct gk20a *g, u32 fifo_intr)
{
	struct device *dev = dev_from_gk20a(g);
	struct fifo_gk20a *f = &g->fifo;
	u32 clear_intr = 0, i;
	u32 pbdma_pending = gk20a_readl(g, fifo_intr_pbdma_id_r());

	for (i = 0; i < fifo_intr_pbdma_id_status__size_1_v(); i++) {
		if (fifo_intr_pbdma_id_status_f(pbdma_pending, i)) {
			nvhost_dbg_fn("pbdma id %d intr pending", i);
			clear_intr |=
				gk20a_fifo_handle_pbdma_intr(dev, g, f, i);
		}
	}
	return fifo_intr_0_pbdma_intr_pending_f();
}

void gk20a_fifo_isr(struct gk20a *g)
{
	u32 error_intr_mask =
		fifo_intr_0_bind_error_pending_f() |
		fifo_intr_0_sched_error_pending_f() |
		fifo_intr_0_chsw_error_pending_f() |
		fifo_intr_0_fb_flush_timeout_pending_f() |
		fifo_intr_0_dropped_mmu_fault_pending_f() |
		fifo_intr_0_mmu_fault_pending_f() |
		fifo_intr_0_lb_error_pending_f() |
		fifo_intr_0_pio_error_pending_f();

	u32 fifo_intr = gk20a_readl(g, fifo_intr_0_r());
	u32 clear_intr = 0;

	/* note we're not actually in an "isr", but rather
	 * in a threaded interrupt context... */
	mutex_lock(&g->fifo.intr.isr.mutex);


	/* handle runlist update */
	if (fifo_intr & fifo_intr_0_runlist_event_pending_f()) {
		gk20a_fifo_handle_runlist_event(g);
		clear_intr |= fifo_intr_0_runlist_event_pending_f();
	}
	if (fifo_intr & fifo_intr_0_pbdma_intr_pending_f())
		clear_intr |= fifo_pbdma_isr(g, fifo_intr);

	if (fifo_intr & fifo_intr_0_channel_intr_pending_f())
		clear_intr |= fifo_channel_isr(g, fifo_intr);

	if (unlikely(fifo_intr & error_intr_mask))
		clear_intr = fifo_error_isr(g, fifo_intr);

	gk20a_writel(g, fifo_intr_0_r(), clear_intr);

	mutex_unlock(&g->fifo.intr.isr.mutex);

	return;
}

int gk20a_fifo_preempt_channel(struct gk20a *g, u32 hw_chid)
{
	struct fifo_gk20a *f = &g->fifo;
	unsigned long end_jiffies = jiffies
		+ msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 delay = GR_IDLE_CHECK_DEFAULT;
	u32 ret = 0;
	u32 token = PMU_INVALID_MUTEX_OWNER_ID;
	u32 elpg_off = 0;
	u32 i;

	nvhost_dbg_fn("%d", hw_chid);

	/* we have no idea which runlist we are using. lock all */
	for (i = 0; i < g->fifo.max_runlists; i++)
		mutex_lock(&f->runlist_info[i].mutex);

	/* disable elpg if failed to acquire pmu mutex */
	elpg_off = pmu_mutex_acquire(&g->pmu, PMU_MUTEX_ID_FIFO, &token);
	if (elpg_off)
		gk20a_pmu_disable_elpg(g);

	/* issue preempt */
	gk20a_writel(g, fifo_preempt_r(),
		fifo_preempt_chid_f(hw_chid) |
		fifo_preempt_type_channel_f());

	/* wait for preempt */
	ret = -EBUSY;
	do {
		if (!(gk20a_readl(g, fifo_preempt_r()) &
			fifo_preempt_pending_true_f())) {
			ret = 0;
			break;
		}

		usleep_range(delay, delay * 2);
		delay = min_t(u32, delay << 1, GR_IDLE_CHECK_MAX);
	} while (time_before(jiffies, end_jiffies) |
			!tegra_platform_is_silicon());

	if (ret) {
		int i;
		u32 engines = 0;

		nvhost_err(dev_from_gk20a(g), "preempt channel %d timeout\n",
			    hw_chid);

		/* forcefully reset all busy engines using this channel */
		for (i = 0; i < g->fifo.max_engines; i++) {
			u32 status = gk20a_readl(g, fifo_engine_status_r(i));
			u32 ctx_status =
				fifo_engine_status_ctx_status_v(status);
			bool type_ch = fifo_pbdma_status_id_type_v(status) ==
				fifo_pbdma_status_id_type_chid_v();
			bool busy = fifo_engine_status_engine_v(status) ==
				fifo_engine_status_engine_busy_v();
			u32 id = (ctx_status ==
				fifo_engine_status_ctx_status_ctxsw_load_v()) ?
				fifo_engine_status_next_id_v(status) :
				fifo_engine_status_id_v(status);

			if (type_ch && busy && id == hw_chid)
				engines |= BIT(i);
		}
		gk20a_fifo_recover(g, engines);
	}

	/* re-enable elpg or release pmu mutex */
	if (elpg_off)
		gk20a_pmu_enable_elpg(g);
	else
		pmu_mutex_release(&g->pmu, PMU_MUTEX_ID_FIFO, &token);

	for (i = 0; i < g->fifo.max_runlists; i++)
		mutex_unlock(&f->runlist_info[i].mutex);

	return ret;
}

int gk20a_fifo_enable_engine_activity(struct gk20a *g,
				struct fifo_engine_info_gk20a *eng_info)
{
	u32 token = PMU_INVALID_MUTEX_OWNER_ID;
	u32 elpg_off;
	u32 enable;

	nvhost_dbg_fn("");

	/* disable elpg if failed to acquire pmu mutex */
	elpg_off = pmu_mutex_acquire(&g->pmu, PMU_MUTEX_ID_FIFO, &token);
	if (elpg_off)
		gk20a_pmu_disable_elpg(g);

	enable = gk20a_readl(g, fifo_sched_disable_r());
	enable &= ~(fifo_sched_disable_true_v() >> eng_info->runlist_id);
	gk20a_writel(g, fifo_sched_disable_r(), enable);

	/* re-enable elpg or release pmu mutex */
	if (elpg_off)
		gk20a_pmu_enable_elpg(g);
	else
		pmu_mutex_release(&g->pmu, PMU_MUTEX_ID_FIFO, &token);

	nvhost_dbg_fn("done");
	return 0;
}

int gk20a_fifo_disable_engine_activity(struct gk20a *g,
				struct fifo_engine_info_gk20a *eng_info,
				bool wait_for_idle)
{
	u32 gr_stat, pbdma_stat, chan_stat, eng_stat, ctx_stat;
	u32 pbdma_chid = ~0, engine_chid = ~0, disable;
	u32 token = PMU_INVALID_MUTEX_OWNER_ID;
	u32 elpg_off;
	u32 err = 0;

	nvhost_dbg_fn("");

	gr_stat =
		gk20a_readl(g, fifo_engine_status_r(eng_info->engine_id));
	if (fifo_engine_status_engine_v(gr_stat) ==
	    fifo_engine_status_engine_busy_v() && !wait_for_idle)
		return -EBUSY;

	/* disable elpg if failed to acquire pmu mutex */
	elpg_off = pmu_mutex_acquire(&g->pmu, PMU_MUTEX_ID_FIFO, &token);
	if (elpg_off)
		gk20a_pmu_disable_elpg(g);

	disable = gk20a_readl(g, fifo_sched_disable_r());
	disable = set_field(disable,
			fifo_sched_disable_runlist_m(eng_info->runlist_id),
			fifo_sched_disable_runlist_f(fifo_sched_disable_true_v(),
				eng_info->runlist_id));
	gk20a_writel(g, fifo_sched_disable_r(), disable);

	/* chid from pbdma status */
	pbdma_stat = gk20a_readl(g, fifo_pbdma_status_r(eng_info->pbdma_id));
	chan_stat  = fifo_pbdma_status_chan_status_v(pbdma_stat);
	if (chan_stat == fifo_pbdma_status_chan_status_valid_v() ||
	    chan_stat == fifo_pbdma_status_chan_status_chsw_save_v())
		pbdma_chid = fifo_pbdma_status_id_v(pbdma_stat);
	else if (chan_stat == fifo_pbdma_status_chan_status_chsw_load_v() ||
		 chan_stat == fifo_pbdma_status_chan_status_chsw_switch_v())
		pbdma_chid = fifo_pbdma_status_next_id_v(pbdma_stat);

	if (pbdma_chid != ~0) {
		err = gk20a_fifo_preempt_channel(g, pbdma_chid);
		if (err)
			goto clean_up;
	}

	/* chid from engine status */
	eng_stat = gk20a_readl(g, fifo_engine_status_r(eng_info->engine_id));
	ctx_stat  = fifo_engine_status_ctx_status_v(eng_stat);
	if (ctx_stat == fifo_engine_status_ctx_status_valid_v() ||
	    ctx_stat == fifo_engine_status_ctx_status_ctxsw_save_v())
		engine_chid = fifo_engine_status_id_v(eng_stat);
	else if (ctx_stat == fifo_engine_status_ctx_status_ctxsw_load_v() ||
		 ctx_stat == fifo_engine_status_ctx_status_ctxsw_switch_v())
		engine_chid = fifo_engine_status_next_id_v(eng_stat);

	if (engine_chid != ~0 && engine_chid != pbdma_chid) {
		err = gk20a_fifo_preempt_channel(g, engine_chid);
		if (err)
			goto clean_up;
	}

clean_up:
	/* re-enable elpg or release pmu mutex */
	if (elpg_off)
		gk20a_pmu_enable_elpg(g);
	else
		pmu_mutex_release(&g->pmu, PMU_MUTEX_ID_FIFO, &token);

	if (err) {
		nvhost_dbg_fn("failed");
		gk20a_fifo_enable_engine_activity(g, eng_info);
	} else {
		nvhost_dbg_fn("done");
	}
	return err;
}

static void gk20a_fifo_runlist_reset_engines(struct gk20a *g, u32 runlist_id)
{
	struct fifo_gk20a *f = &g->fifo;
	u32 engines = 0;
	int i;

	for (i = 0; i < f->max_engines; i++) {
		u32 status = gk20a_readl(g, fifo_engine_status_r(i));
		bool engine_busy = fifo_engine_status_engine_v(status) ==
			fifo_engine_status_engine_busy_v();

		if (engine_busy &&
		    (f->engine_info[i].runlist_id == runlist_id))
			engines |= BIT(i);
	}
	gk20a_fifo_recover(g, engines);
}

static int gk20a_fifo_runlist_wait_pending(struct gk20a *g, u32 runlist_id)
{
	struct fifo_runlist_info_gk20a *runlist;
	u32 remain;
	bool pending;

	runlist = &g->fifo.runlist_info[runlist_id];

	remain = wait_event_timeout(runlist->runlist_wq,
		((pending = gk20a_readl(g, fifo_eng_runlist_r(runlist_id)) &
			fifo_eng_runlist_pending_true_f()) == 0),
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g)));

	if (remain == 0 && pending != 0)
		return -ETIMEDOUT;
	else if (remain < 0)
		return -EINTR;

	return 0;
}

static int gk20a_fifo_update_runlist_locked(struct gk20a *g, u32 runlist_id,
					    u32 hw_chid, bool add,
					    bool wait_for_finish)
{
	u32 ret = 0;
	struct fifo_gk20a *f = &g->fifo;
	struct fifo_runlist_info_gk20a *runlist = NULL;
	u32 *runlist_entry_base = NULL;
	u32 *runlist_entry = NULL;
	phys_addr_t runlist_pa;
	u32 old_buf, new_buf;
	u32 chid;
	u32 count = 0;

	runlist = &f->runlist_info[runlist_id];

	/* valid channel, add/remove it from active list.
	   Otherwise, keep active list untouched for suspend/resume. */
	if (hw_chid != ~0) {
		if (add) {
			if (test_and_set_bit(hw_chid,
				runlist->active_channels) == 1)
				return 0;
		} else {
			if (test_and_clear_bit(hw_chid,
				runlist->active_channels) == 0)
				return 0;
		}
	}

	old_buf = runlist->cur_buffer;
	new_buf = !runlist->cur_buffer;

	nvhost_dbg_info("runlist_id : %d, switch to new buffer %p",
		runlist_id, runlist->mem[new_buf].ref);

	runlist_pa = sg_phys(runlist->mem[new_buf].sgt->sgl);

	runlist_entry_base = nvhost_memmgr_mmap(runlist->mem[new_buf].ref);
	if (!runlist_entry_base) {
		ret = -ENOMEM;
		goto clean_up;
	}

	if (hw_chid != ~0 || /* add/remove a valid channel */
	    add /* resume to add all channels back */) {
		runlist_entry = runlist_entry_base;
		for_each_set_bit(chid,
			runlist->active_channels, f->num_channels) {
			nvhost_dbg_info("add channel %d to runlist", chid);
			runlist_entry[0] = chid;
			runlist_entry[1] = 0;
			runlist_entry += 2;
			count++;
		}
	} else	/* suspend to remove all channels */
		count = 0;

	if (count != 0) {
		gk20a_writel(g, fifo_runlist_base_r(),
			fifo_runlist_base_ptr_f(u64_lo32(runlist_pa >> 12)) |
			fifo_runlist_base_target_vid_mem_f());
	}

	gk20a_writel(g, fifo_runlist_r(),
		fifo_runlist_engine_f(runlist_id) |
		fifo_eng_runlist_length_f(count));

	if (wait_for_finish) {
		ret = gk20a_fifo_runlist_wait_pending(g, runlist_id);

		if (ret == -ETIMEDOUT) {
			nvhost_err(dev_from_gk20a(g),
				   "runlist update timeout");

			gk20a_fifo_runlist_reset_engines(g, runlist_id);
			ret = gk20a_fifo_runlist_wait_pending(g, runlist_id);
			if (ret)
				nvhost_err(dev_from_gk20a(g),
					   "runlist update failed: %d", ret);
		} else if (ret == -EINTR)
			nvhost_err(dev_from_gk20a(g),
				   "runlist update interrupted");
	}

	runlist->cur_buffer = new_buf;

clean_up:
	nvhost_memmgr_munmap(runlist->mem[new_buf].ref,
			     runlist_entry_base);

	return ret;
}

/* add/remove a channel from runlist
   special cases below: runlist->active_channels will NOT be changed.
   (hw_chid == ~0 && !add) means remove all active channels from runlist.
   (hw_chid == ~0 &&  add) means restore all active channels on runlist. */
int gk20a_fifo_update_runlist(struct gk20a *g, u32 runlist_id, u32 hw_chid,
			      bool add, bool wait_for_finish)
{
	struct fifo_runlist_info_gk20a *runlist = NULL;
	struct fifo_gk20a *f = &g->fifo;
	u32 token = PMU_INVALID_MUTEX_OWNER_ID;
	u32 elpg_off;
	u32 ret = 0;

	runlist = &f->runlist_info[runlist_id];

	mutex_lock(&runlist->mutex);

	/* disable elpg if failed to acquire pmu mutex */
	elpg_off = pmu_mutex_acquire(&g->pmu, PMU_MUTEX_ID_FIFO, &token);
	if (elpg_off)
		gk20a_pmu_disable_elpg(g);

	ret = gk20a_fifo_update_runlist_locked(g, runlist_id, hw_chid, add,
					       wait_for_finish);

	/* re-enable elpg or release pmu mutex */
	if (elpg_off)
		gk20a_pmu_enable_elpg(g);
	else
		pmu_mutex_release(&g->pmu, PMU_MUTEX_ID_FIFO, &token);

	mutex_unlock(&runlist->mutex);
	return ret;
}

int gk20a_fifo_suspend(struct gk20a *g)
{
	nvhost_dbg_fn("");

	/* stop bar1 snooping */
	gk20a_writel(g, fifo_bar1_base_r(),
			fifo_bar1_base_valid_false_f());

	/* disable fifo intr */
	gk20a_writel(g, fifo_intr_en_0_r(), 0);
	gk20a_writel(g, fifo_intr_en_1_r(), 0);

	nvhost_dbg_fn("done");
	return 0;
}
