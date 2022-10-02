/*
 * Tegra GK20A GPU Debugger/Profiler Driver
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

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/nvhost.h>
#include <linux/nvhost_dbg_gpu_ioctl.h>

#include "dev.h"
#include "nvhost_hwctx.h"
#include "nvhost_acm.h"
#include "gk20a.h"
#include "gr_gk20a.h"
#include "gk20a_gating_reglist.h"
#include "dbg_gpu_gk20a.h"
#include "regops_gk20a.h"

struct dbg_gpu_session_ops dbg_gpu_session_ops_gk20a = {
	.exec_reg_ops = exec_regops_gk20a
};

/* silly allocator - just increment session id */
static atomic_t session_id = ATOMIC_INIT(0);
static int generate_session_id(void)
{
	return atomic_add_return(1, &session_id);
}

static int alloc_session(struct dbg_session_gk20a **_dbg_s)
{
	struct dbg_session_gk20a *dbg_s;
	*_dbg_s = NULL;

	nvhost_dbg_fn("");

	dbg_s = kzalloc(sizeof(*dbg_s), GFP_KERNEL);
	if (!dbg_s)
		return -ENOMEM;

	dbg_s->id = generate_session_id();
	dbg_s->ops = &dbg_gpu_session_ops_gk20a;
	*_dbg_s = dbg_s;
	return 0;
}

int gk20a_dbg_gpu_do_dev_open(struct inode *inode, struct file *filp, bool is_profiler)
{
	struct dbg_session_gk20a *dbg_session;
	struct nvhost_device_data *pdata;

	struct platform_device *pdev;
	struct device *dev;

	int err;

	if (!is_profiler)
		pdata = container_of(inode->i_cdev,
			     struct nvhost_device_data, dbg_cdev);
	else
		pdata = container_of(inode->i_cdev,
				     struct nvhost_device_data, prof_cdev);
	pdev = pdata->pdev;
	dev  = &pdev->dev;

	nvhost_dbg(dbg_fn | dbg_gpu_dbg, "dbg session: %s", dev_name(dev));

	err  = alloc_session(&dbg_session);
	if (err)
		return err;

	filp->private_data = dbg_session;
	dbg_session->pdata = pdata;
	dbg_session->pdev  = pdev;
	dbg_session->dev   = dev;
	dbg_session->g     = get_gk20a(pdev);
	dbg_session->is_profiler = is_profiler;

	return 0;
}

int gk20a_dbg_gpu_dev_open(struct inode *inode, struct file *filp)
{
	nvhost_dbg_fn("");
	return gk20a_dbg_gpu_do_dev_open(inode, filp, false /* not profiler */);
}

int gk20a_prof_gpu_dev_open(struct inode *inode, struct file *filp)
{
	nvhost_dbg_fn("");
	return gk20a_dbg_gpu_do_dev_open(inode, filp, true /* is profiler */);
}

static int dbg_unbind_channel_gk20a(struct dbg_session_gk20a *dbg_s)
{
	struct channel_gk20a *ch_gk20a = dbg_s->ch;
	struct gk20a *g = dbg_s->g;

	nvhost_dbg_fn("");

	/* wasn't bound to start with ? */
	if (!ch_gk20a) {
		nvhost_dbg(dbg_gpu_dbg | dbg_fn, "not bound already?");
		return -ENODEV;
	}

	mutex_lock(&g->dbg_sessions_lock);
	mutex_lock(&ch_gk20a->dbg_s_lock);

	if (--g->dbg_sessions == 0) {
		/* restore (can) powergate, clk state */
		/* release pending exceptions to fault/be handled as usual */
		/*TBD: ordering of these? */
		g->elcg_enabled = true;
		gr_gk20a_init_elcg_mode(g, ELCG_AUTO, ENGINE_GR_GK20A);
		gr_gk20a_init_elcg_mode(g, ELCG_AUTO, ENGINE_CE2_GK20A);

		gr_gk20a_blcg_gr_load_gating_prod(g, g->blcg_enabled);
		/* ???  gr_gk20a_pg_gr_load_gating_prod(g, true); */

		gr_gk20a_slcg_gr_load_gating_prod(g, g->slcg_enabled);
		gr_gk20a_slcg_perf_load_gating_prod(g, g->slcg_enabled);

		gk20a_pmu_enable_elpg(g);

		nvhost_dbg(dbg_gpu_dbg | dbg_fn, "module idle");
		nvhost_module_idle(dbg_s->pdev);
	}

	ch_gk20a->dbg_s = NULL;
	dbg_s->ch       = NULL;
	fput(dbg_s->hwctx_f);
	dbg_s->hwctx_f   = NULL;

	mutex_unlock(&ch_gk20a->dbg_s_lock);
	mutex_unlock(&g->dbg_sessions_lock);

	return 0;
}

int gk20a_dbg_gpu_dev_release(struct inode *inode, struct file *filp)
{
	struct dbg_session_gk20a *dbg_s = filp->private_data;

	nvhost_dbg(dbg_gpu_dbg | dbg_fn, "%s", dev_name(dbg_s->dev));

	/* unbind if it was bound */
	if (!dbg_s->ch)
		return 0;
	dbg_unbind_channel_gk20a(dbg_s);

	kfree(dbg_s);
	return 0;
}

static int dbg_bind_channel_gk20a(struct dbg_session_gk20a *dbg_s,
			  struct nvhost_dbg_gpu_bind_channel_args *args)
{
	struct file *f;
	struct nvhost_hwctx *hwctx;
	struct gk20a *g;
	struct channel_gk20a *ch_gk20a;

	nvhost_dbg(dbg_fn|dbg_gpu_dbg, "%s fd=%d",
		   dev_name(dbg_s->dev), args->channel_fd);

	if (args->channel_fd == ~0)
		return dbg_unbind_channel_gk20a(dbg_s);

	/* even though get_file_hwctx is doing this it releases it as well */
	/* by holding it here we'll keep it from disappearing while the
	 * debugger is in session */
	f = fget(args->channel_fd);
	if (!f)
		return -ENODEV;

	hwctx = nvhost_channel_get_file_hwctx(args->channel_fd);
	if (!hwctx) {
		nvhost_dbg_fn("no hwctx found for fd");
		fput(f);
		return -EINVAL;
	}
	/* be sure this is actually the right type of hwctx */
	if (hwctx->channel->dev != dbg_s->pdev) {
		nvhost_dbg_fn("hwctx module type mismatch");
		fput(f);
		return -EINVAL;
	}
	if (!hwctx->priv) {
		nvhost_dbg_fn("no priv");
		fput(f);
		return -ENODEV;
	}

	ch_gk20a = (struct channel_gk20a *)hwctx->priv;
	g = dbg_s->g;
	nvhost_dbg_fn("%s hwchid=%d", dev_name(dbg_s->dev), ch_gk20a->hw_chid);

	mutex_lock(&g->dbg_sessions_lock);
	mutex_lock(&ch_gk20a->dbg_s_lock);

	if (ch_gk20a->dbg_s) {
		mutex_unlock(&ch_gk20a->dbg_s_lock);
		mutex_unlock(&g->dbg_sessions_lock);
		fput(f);
		nvhost_dbg_fn("hwctx already in dbg session");
		return -EBUSY;
	}

	dbg_s->hwctx_f  = f;
	dbg_s->ch       = ch_gk20a;
	ch_gk20a->dbg_s = dbg_s;

	if (g->dbg_sessions++ == 0) {
		/* save off current powergate, clk state.
		 * set gpu module's can_powergate = 0.
		 * set gpu module's clk to max.
		 * while *a* debug session is active there will be no power or
		 * clocking state changes allowed from mainline code (but they
		 * should be saved).
		 */
		nvhost_module_busy(dbg_s->pdev);

		gr_gk20a_slcg_gr_load_gating_prod(g, false);
		gr_gk20a_slcg_perf_load_gating_prod(g, false);

		gr_gk20a_blcg_gr_load_gating_prod(g, false);
		/* ???  gr_gk20a_pg_gr_load_gating_prod(g, false); */
		/* TBD: would rather not change elcg_enabled here */
		g->elcg_enabled = false;
		gr_gk20a_init_elcg_mode(g, ELCG_RUN, ENGINE_GR_GK20A);
		gr_gk20a_init_elcg_mode(g, ELCG_RUN, ENGINE_CE2_GK20A);

		gk20a_pmu_disable_elpg(g);

	}
	mutex_unlock(&ch_gk20a->dbg_s_lock);
	mutex_unlock(&g->dbg_sessions_lock);
	return 0;
}

static int nvhost_ioctl_channel_reg_ops(struct dbg_session_gk20a *dbg_s,
				struct nvhost_dbg_gpu_exec_reg_ops_args *args);

long gk20a_dbg_gpu_dev_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct dbg_session_gk20a *dbg_s = filp->private_data;
	struct gk20a *g = get_gk20a(dbg_s->pdev);
	u8 buf[NVHOST_DBG_GPU_IOCTL_MAX_ARG_SIZE];
	int err = 0;

	nvhost_dbg_fn("");

	if ((_IOC_TYPE(cmd) != NVHOST_DBG_GPU_IOCTL_MAGIC) ||
	    (_IOC_NR(cmd) == 0) ||
	    (_IOC_NR(cmd) > NVHOST_DBG_GPU_IOCTL_LAST))
		return -EFAULT;

	BUG_ON(_IOC_SIZE(cmd) > NVHOST_DBG_GPU_IOCTL_MAX_ARG_SIZE);

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		if (copy_from_user(buf, (void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	switch (cmd) {
	case NVHOST_DBG_GPU_IOCTL_BIND_CHANNEL:
		err = dbg_bind_channel_gk20a(dbg_s,
			     (struct nvhost_dbg_gpu_bind_channel_args *)buf);
		nvhost_dbg(dbg_gpu_dbg, "ret=%d", err);
		break;

	case NVHOST_DBG_GPU_IOCTL_REG_OPS:
		err = nvhost_ioctl_channel_reg_ops(dbg_s,
			   (struct nvhost_dbg_gpu_exec_reg_ops_args *)buf);
		nvhost_dbg(dbg_gpu_dbg, "ret=%d", err);
		break;

	default:
		nvhost_err(dev_from_gk20a(g),
			   "unrecognized dbg gpu ioctl cmd: 0x%x",
			   cmd);
		err = -ENOTTY;
		break;
	}

	if ((err == 0) && (_IOC_DIR(cmd) & _IOC_READ))
		err = copy_to_user((void __user *)arg,
				   buf, _IOC_SIZE(cmd));

	return err;
}

/* In order to perform a context relative op the context has
 * to be created already... which would imply that the
 * context switch mechanism has already been put in place.
 * So by the time we perform such an opertation it should always
 * be possible to query for the appropriate context offsets, etc.
 *
 * But note: while the dbg_gpu bind requires the a channel fd with
 * a bound hwctx it doesn't require an allocated gr/compute obj
 * at that point... so just having the bound hwctx doesn't work
 * to guarantee this.
 */
static bool gr_context_info_available(struct dbg_session_gk20a *dbg_s,
				      struct gr_gk20a *gr)
{
	int err;

	mutex_lock(&gr->ctx_mutex);
	err = !gr->ctx_vars.golden_image_initialized;
	mutex_unlock(&gr->ctx_mutex);
	if (err)
		return false;
	return true;

}

static int nvhost_ioctl_channel_reg_ops(struct dbg_session_gk20a *dbg_s,
				struct nvhost_dbg_gpu_exec_reg_ops_args *args)
{
	int err;
	struct device *dev = dbg_s->dev;
	struct gk20a *g = get_gk20a(dbg_s->pdev);
	struct nvhost_dbg_gpu_reg_op *ops;
	u64 ops_size = sizeof(ops[0]) * args->num_ops;

	nvhost_dbg_fn("%d ops, total size %llu", args->num_ops, ops_size);

	if (!dbg_s->ops) {
		nvhost_err(dev, "can't call reg_ops on an unbound debugger session");
		return -EINVAL;
	}

	/* be sure that ctx info is in place */
	if (!gr_context_info_available(dbg_s, &g->gr)) {
		nvhost_err(dev, "gr context data not available\n");
		return -ENODEV;
	}

	ops = kzalloc(ops_size, GFP_KERNEL);
	if (!ops) {
		nvhost_err(dev, "Allocating memory failed!");
		return -ENOMEM;
	}

	nvhost_dbg_fn("Copying regops from userspace");

	if (copy_from_user(ops, (void *)(uintptr_t)args->ops, ops_size)) {
		dev_err(dev, "copy_from_user failed!");
		return -EFAULT;
	}

	err = dbg_s->ops->exec_reg_ops(dbg_s, ops, args->num_ops);

	if (err) {
		nvhost_err(dev, "dbg regops failed");
		return err;
	}

	nvhost_dbg_fn("Copying result to userspace");

	if (copy_to_user((void *)(uintptr_t)args->ops, ops, ops_size)) {
		dev_err(dev, "copy_to_user failed!");
		return -EFAULT;
	}
	return 0;
}
