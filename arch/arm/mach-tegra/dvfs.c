/*
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * Copyright (C) 2010-2013 NVIDIA CORPORATION. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <linux/clk/tegra.h>
#include <linux/reboot.h>
#include <linux/clk/tegra.h>
#include <linux/tegra-soc.h>

#include "board.h"
#include "clock.h"
#include "dvfs.h"

#define DVFS_RAIL_STATS_BIN	12500

struct dvfs_rail *tegra_cpu_rail;
struct dvfs_rail *tegra_core_rail;
struct dvfs_rail *tegra_gpu_rail;

static LIST_HEAD(dvfs_rail_list);
static DEFINE_MUTEX(dvfs_lock);
static DEFINE_MUTEX(rail_disable_lock);

static int dvfs_rail_update(struct dvfs_rail *rail);

static inline int tegra_dvfs_rail_get_disable_level(struct dvfs_rail *rail)
{
	return rail->disable_millivolts ? : rail->nominal_millivolts;
}

static inline int tegra_dvfs_rail_get_suspend_level(struct dvfs_rail *rail)
{
	return rail->suspend_millivolts ? : rail->nominal_millivolts;
}

void tegra_dvfs_add_relationships(struct dvfs_relationship *rels, int n)
{
	int i;
	struct dvfs_relationship *rel;

	mutex_lock(&dvfs_lock);

	for (i = 0; i < n; i++) {
		rel = &rels[i];
		list_add_tail(&rel->from_node, &rel->to->relationships_from);
		list_add_tail(&rel->to_node, &rel->from->relationships_to);
	}

	mutex_unlock(&dvfs_lock);
}

/* Make sure there is a matching cooling device for thermal limit profile. */
static void dvfs_validate_cdevs(struct dvfs_rail *rail)
{
	if (!rail->therm_mv_caps != !rail->therm_mv_caps_num) {
		rail->therm_mv_caps_num = 0;
		rail->therm_mv_caps = NULL;
		WARN(1, "%s: not matching thermal caps/num\n", rail->reg_id);
	}

	if (rail->therm_mv_caps && !rail->vmax_cdev)
		WARN(1, "%s: missing vmax cooling device\n", rail->reg_id);

	if (!rail->therm_mv_floors != !rail->therm_mv_floors_num) {
		rail->therm_mv_floors_num = 0;
		rail->therm_mv_floors = NULL;
		WARN(1, "%s: not matching thermal floors/num\n", rail->reg_id);
	}

	if (rail->therm_mv_floors && !rail->vmin_cdev)
		WARN(1, "%s: missing vmin cooling device\n", rail->reg_id);

	/* Limit override range to maximum floor */
	if (rail->therm_mv_floors)
		rail->min_override_millivolts = rail->therm_mv_floors[0];

	/* Only GPU thermal dvfs is supported */
	if (rail->vts_cdev && (rail != tegra_gpu_rail)) {
		rail->vts_cdev = NULL;
		WARN(1, "%s: thermal dvfs is not supported\n", rail->reg_id);
	}
}

int tegra_dvfs_init_rails(struct dvfs_rail *rails[], int n)
{
	int i, mv;

	mutex_lock(&dvfs_lock);

	for (i = 0; i < n; i++) {
		INIT_LIST_HEAD(&rails[i]->dvfs);
		INIT_LIST_HEAD(&rails[i]->relationships_from);
		INIT_LIST_HEAD(&rails[i]->relationships_to);

		mv = rails[i]->nominal_millivolts;
		if (rails[i]->boot_millivolts > mv)
			WARN(1, "%s: boot voltage %d above nominal %d\n",
			     rails[i]->reg_id, rails[i]->boot_millivolts, mv);
		if (rails[i]->disable_millivolts > mv)
			rails[i]->disable_millivolts = mv;
		if (rails[i]->suspend_millivolts > mv)
			rails[i]->suspend_millivolts = mv;

		mv = tegra_dvfs_rail_get_boot_level(rails[i]);
		rails[i]->millivolts = mv;
		rails[i]->new_millivolts = mv;
		if (!rails[i]->step)
			rails[i]->step = rails[i]->max_millivolts;
		if (!rails[i]->step_up)
			rails[i]->step_up = rails[i]->step;

		list_add_tail(&rails[i]->node, &dvfs_rail_list);

		if (!strcmp("vdd_cpu", rails[i]->reg_id))
			tegra_cpu_rail = rails[i];
		else if (!strcmp("vdd_gpu", rails[i]->reg_id))
			tegra_gpu_rail = rails[i];
		else if (!strcmp("vdd_core", rails[i]->reg_id))
			tegra_core_rail = rails[i];

		dvfs_validate_cdevs(rails[i]);
	}

	mutex_unlock(&dvfs_lock);

	return 0;
};

static int dvfs_solve_relationship(struct dvfs_relationship *rel)
{
	return rel->solve(rel->from, rel->to);
}

/* rail statistic - called during rail init, or under dfs_lock, or with
   CPU0 only on-line, and interrupts disabled */
static void dvfs_rail_stats_init(struct dvfs_rail *rail, int millivolts)
{
	int dvfs_rail_stats_range;

	if (!rail->stats.bin_uV)
		rail->stats.bin_uV = DVFS_RAIL_STATS_BIN;

	dvfs_rail_stats_range =
		(DVFS_RAIL_STATS_TOP_BIN - 1) * rail->stats.bin_uV / 1000;

	rail->stats.last_update = ktime_get();
	if (millivolts >= rail->min_millivolts) {
		int i = 1 + (2 * (millivolts - rail->min_millivolts) * 1000 +
			     rail->stats.bin_uV) / (2 * rail->stats.bin_uV);
		rail->stats.last_index = min(i, DVFS_RAIL_STATS_TOP_BIN);
	}

	if (rail->max_millivolts >
	    rail->min_millivolts + dvfs_rail_stats_range)
		pr_warn("tegra_dvfs: %s: stats above %d mV will be squashed\n",
			rail->reg_id,
			rail->min_millivolts + dvfs_rail_stats_range);
}

static void dvfs_rail_stats_update(
	struct dvfs_rail *rail, int millivolts, ktime_t now)
{
	rail->stats.time_at_mv[rail->stats.last_index] = ktime_add(
		rail->stats.time_at_mv[rail->stats.last_index], ktime_sub(
			now, rail->stats.last_update));
	rail->stats.last_update = now;

	if (rail->stats.off)
		return;

	if (millivolts >= rail->min_millivolts) {
		int i = 1 + (2 * (millivolts - rail->min_millivolts) * 1000 +
			     rail->stats.bin_uV) / (2 * rail->stats.bin_uV);
		rail->stats.last_index = min(i, DVFS_RAIL_STATS_TOP_BIN);
	} else if (millivolts == 0)
			rail->stats.last_index = 0;
}

static void dvfs_rail_stats_pause(struct dvfs_rail *rail,
				  ktime_t delta, bool on)
{
	int i = on ? rail->stats.last_index : 0;
	rail->stats.time_at_mv[i] = ktime_add(rail->stats.time_at_mv[i], delta);
}

void tegra_dvfs_rail_off(struct dvfs_rail *rail, ktime_t now)
{
	if (rail) {
		dvfs_rail_stats_update(rail, 0, now);
		rail->stats.off = true;
	}
}

void tegra_dvfs_rail_on(struct dvfs_rail *rail, ktime_t now)
{
	if (rail) {
		rail->stats.off = false;
		dvfs_rail_stats_update(rail, rail->millivolts, now);
	}
}

void tegra_dvfs_rail_pause(struct dvfs_rail *rail, ktime_t delta, bool on)
{
	if (rail)
		dvfs_rail_stats_pause(rail, delta, on);
}

static int dvfs_rail_set_voltage_reg(struct dvfs_rail *rail, int millivolts)
{
	int ret;

	/*
	 * safely return success for low voltage requests on fixed regulator
	 * (higher requests will go through and fail, as they should)
	 */
	if (rail->fixed_millivolts && (millivolts <= rail->fixed_millivolts))
		return 0;

	rail->updating = true;
	rail->reg_max_millivolts = rail->reg_max_millivolts ==
		rail->max_millivolts ?
		rail->max_millivolts + 1 : rail->max_millivolts;
	ret = regulator_set_voltage(rail->reg,
		millivolts * 1000,
		rail->reg_max_millivolts * 1000);
	rail->updating = false;

	return ret;
}

/* Sets the voltage on a dvfs rail to a specific value, and updates any
 * rails that depend on this rail. */
static int dvfs_rail_set_voltage(struct dvfs_rail *rail, int millivolts)
{
	int ret = 0;
	struct dvfs_relationship *rel;
	int step, offset;
	int i;
	int steps;
	bool jmp_to_zero;

	if (!rail->reg) {
		if (millivolts == rail->millivolts)
			return 0;
		else
			return -EINVAL;
	}

	if (millivolts > rail->millivolts) {
		step = rail->step_up;
		offset = step;
	} else {
		step = rail->step;
		offset = -step;
	}

	/*
	 * DFLL adjusts rail voltage automatically, but not exactly to the
	 * expected level - update stats, anyway.
	 */
	if (rail->dfll_mode) {
		rail->millivolts = rail->new_millivolts = millivolts;
		dvfs_rail_stats_update(rail, millivolts, ktime_get());
		return 0;
	}

	if (rail->disabled)
		return 0;

	rail->resolving_to = true;
	jmp_to_zero = rail->jmp_to_zero &&
			((millivolts == 0) || (rail->millivolts == 0));
	steps = jmp_to_zero ? 1 :
		DIV_ROUND_UP(abs(millivolts - rail->millivolts), step);

	for (i = 0; i < steps; i++) {
		if (!jmp_to_zero &&
		    (abs(millivolts - rail->millivolts) > step))
			rail->new_millivolts = rail->millivolts + offset;
		else
			rail->new_millivolts = millivolts;

		/* Before changing the voltage, tell each rail that depends
		 * on this rail that the voltage will change.
		 * This rail will be the "from" rail in the relationship,
		 * the rail that depends on this rail will be the "to" rail.
		 * from->millivolts will be the old voltage
		 * from->new_millivolts will be the new voltage */
		list_for_each_entry(rel, &rail->relationships_to, to_node) {
			ret = dvfs_rail_update(rel->to);
			if (ret)
				goto out;
		}

		ret = dvfs_rail_set_voltage_reg(rail, rail->new_millivolts);
		if (ret) {
			pr_err("Failed to set dvfs regulator %s\n", rail->reg_id);
			goto out;
		}

		rail->millivolts = rail->new_millivolts;
		dvfs_rail_stats_update(rail, rail->millivolts, ktime_get());

		/* After changing the voltage, tell each rail that depends
		 * on this rail that the voltage has changed.
		 * from->millivolts and from->new_millivolts will be the
		 * new voltage */
		list_for_each_entry(rel, &rail->relationships_to, to_node) {
			ret = dvfs_rail_update(rel->to);
			if (ret)
				goto out;
		}
	}

	if (unlikely(rail->millivolts != millivolts)) {
		pr_err("%s: rail didn't reach target %d in %d steps (%d)\n",
			__func__, millivolts, steps, rail->millivolts);
		ret = -EINVAL;
	}

out:
	rail->resolving_to = false;
	return ret;
}

/* Determine the minimum valid voltage for a rail, taking into account
 * the dvfs clocks and any rails that this rail depends on.  Calls
 * dvfs_rail_set_voltage with the new voltage, which will call
 * dvfs_rail_update on any rails that depend on this rail. */
static inline int dvfs_rail_apply_limits(struct dvfs_rail *rail, int millivolts)
{
	int min_mv = rail->min_millivolts;

	if (rail->therm_mv_floors) {
		int i = rail->therm_floor_idx;
		if (i < rail->therm_mv_floors_num)
			min_mv = rail->therm_mv_floors[i];
	}

	if (rail->override_millivolts) {
		millivolts = rail->override_millivolts;
	} else {
		/* apply offset and clip up to pll mode fixed mv */
		millivolts += rail->dbg_mv_offs;
		if (!rail->dfll_mode && rail->fixed_millivolts &&
		    (millivolts < rail->fixed_millivolts))
			millivolts = rail->fixed_millivolts;
	}

	if (millivolts < min_mv)
		millivolts = min_mv;

	return millivolts;
}

static int dvfs_rail_update(struct dvfs_rail *rail)
{
	int millivolts = 0;
	struct dvfs *d;
	struct dvfs_relationship *rel;
	int ret = 0;
	int steps;

	/* if dvfs is suspended, return and handle it during resume */
	if (rail->suspended)
		return 0;

	/* if regulators are not connected yet, return and handle it later */
	if (!rail->reg)
		return 0;

	/* if no clock has requested voltage since boot, defer update */
	if (!rail->rate_set)
		return 0;

	/* if rail update is entered while resolving circular dependencies,
	   abort recursion */
	if (rail->resolving_to)
		return 0;

	/* Find the maximum voltage requested by any clock */
	list_for_each_entry(d, &rail->dvfs, reg_node)
		millivolts = max(d->cur_millivolts, millivolts);

	/* Apply offset and min/max limits if any clock is requesting voltage */
	if (millivolts)
		millivolts = dvfs_rail_apply_limits(rail, millivolts);
	/* Keep current voltage if regulator is to be disabled via explicitly */
	else if (rail->in_band_pm)
		return 0;
	/* Keep current voltage if regulator must not be disabled at run time */
	else if (!rail->jmp_to_zero) {
		WARN(1, "%s cannot be turned off by dvfs\n", rail->reg_id);
		return 0;
	}
	/* else: fall thru if regulator is turned off by side band signaling */

	/* retry update if limited by from-relationship to account for
	   circular dependencies */
	steps = DIV_ROUND_UP(abs(millivolts - rail->millivolts), rail->step);
	for (; steps >= 0; steps--) {
		rail->new_millivolts = millivolts;

		/* Check any rails that this rail depends on */
		list_for_each_entry(rel, &rail->relationships_from, from_node)
			rail->new_millivolts = dvfs_solve_relationship(rel);

		if (rail->new_millivolts == rail->millivolts)
			break;

		ret = dvfs_rail_set_voltage(rail, rail->new_millivolts);
	}

	return ret;
}

static struct regulator *get_fixed_regulator(struct dvfs_rail *rail)
{
	struct regulator *reg;
	char reg_id[80];
	struct dvfs *d;
	int v, i;
	unsigned long dfll_boost;

	strcpy(reg_id, rail->reg_id);
	strcat(reg_id, "_fixed");
	reg = regulator_get(NULL, reg_id);
	if (IS_ERR(reg))
		return reg;

	v = regulator_get_voltage(reg) / 1000;
	if ((v < rail->min_millivolts) || (v > rail->nominal_millivolts) ||
	    (rail->therm_mv_floors && v < rail->therm_mv_floors[0])) {
		pr_err("tegra_dvfs: ivalid fixed %s voltage %d\n",
		       rail->reg_id, v);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * Only fixed at nominal voltage vdd_core regulator is allowed, same
	 * is true for cpu rail if dfll mode is not supported at all. No thermal
	 * capping can be implemented in this case.
	 */
	if (!IS_ENABLED(CONFIG_ARCH_TEGRA_HAS_CL_DVFS) ||
	    (rail != tegra_cpu_rail)) {
		if (v != rail->nominal_millivolts) {
			pr_err("tegra_dvfs: %s fixed below nominal at %d\n",
			       rail->reg_id, v);
			return ERR_PTR(-EINVAL);
		}
		if (rail->therm_mv_caps) {
			pr_err("tegra_dvfs: cannot fix %s with thermal caps\n",
			       rail->reg_id);
			return ERR_PTR(-ENOSYS);
		}
		return reg;
	}

	/*
	 * If dfll mode is supported, fixed vdd_cpu regulator may be below
	 * nominal in pll mode - maximum cpu rate in pll mode is limited
	 * respectively. Regulator is required to allow automatic scaling
	 * in dfll mode.
	 *
	 * FIXME: platform data to explicitly identify such "hybrid" regulator?
	 */
	d = list_first_entry(&rail->dvfs, struct dvfs, reg_node);
	for (i = 0; i < d->num_freqs; i++) {
		if (d->millivolts[i] > v)
			break;
	}

	if (!i) {
		pr_err("tegra_dvfs: %s fixed at %d: too low for min rate\n",
		       rail->reg_id, v);
		return ERR_PTR(-EINVAL);
	}

	dfll_boost = (d->freqs[d->num_freqs - 1] - d->freqs[i - 1]);
	if (d->dfll_data.max_rate_boost < dfll_boost)
		d->dfll_data.max_rate_boost = dfll_boost;

	rail->fixed_millivolts = v;
	return reg;
}

static int dvfs_rail_connect_to_regulator(struct dvfs_rail *rail)
{
	struct regulator *reg;
	int v;

	if (!rail->reg) {
		reg = regulator_get(NULL, rail->reg_id);
		if (IS_ERR(reg)) {
			reg = get_fixed_regulator(rail);
			if (IS_ERR(reg)) {
				pr_err("tegra_dvfs: failed to connect %s rail\n",
				       rail->reg_id);
				return PTR_ERR(reg);
			}
		}
		rail->reg = reg;
	}

	v = regulator_enable(rail->reg);
	if (v < 0) {
		pr_err("tegra_dvfs: failed on enabling regulator %s\n, err %d",
			rail->reg_id, v);
		return v;
	}

	v = regulator_get_voltage(rail->reg);
	if (v < 0) {
		pr_err("tegra_dvfs: failed initial get %s voltage\n",
		       rail->reg_id);
		return v;
	}
	rail->millivolts = v / 1000;
	rail->new_millivolts = rail->millivolts;
	dvfs_rail_stats_init(rail, rail->millivolts);

	if (rail->boot_millivolts &&
	    (rail->boot_millivolts != rail->millivolts)) {
		WARN(1, "%s boot voltage %d does not match expected %d\n",
		     rail->reg_id, rail->millivolts, rail->boot_millivolts);
		rail->boot_millivolts = rail->millivolts;
	}
	return 0;
}

static inline unsigned long *dvfs_get_freqs(struct dvfs *d)
{
	return d->alt_freqs ? : &d->freqs[0];
}

static inline const int *dvfs_get_millivolts(struct dvfs *d, unsigned long rate)
{
	if (tegra_dvfs_is_dfll_scale(d, rate))
		return d->dfll_millivolts;

	return tegra_dvfs_get_millivolts_pll(d);
}

static int
__tegra_dvfs_set_rate(struct dvfs *d, unsigned long rate)
{
	int i = 0;
	int ret, mv, detach_mv;
	unsigned long *freqs = dvfs_get_freqs(d);
	const int *millivolts = dvfs_get_millivolts(d, rate);

	if (freqs == NULL || millivolts == NULL)
		return -ENODEV;

	/* On entry to dfll range limit 1st step to range bottom (full ramp of
	   voltage/rate is completed automatically in dfll mode) */
	if (tegra_dvfs_is_dfll_range_entry(d, rate))
		rate = d->dfll_data.use_dfll_rate_min;

	if (rate > freqs[d->num_freqs - 1]) {
		pr_warn("tegra_dvfs: rate %lu too high for dvfs on %s\n", rate,
			d->clk_name);
		return -EINVAL;
	}

	if (rate == 0) {
		d->cur_millivolts = 0;
	} else {
		while (i < d->num_freqs && rate > freqs[i])
			i++;

		if ((d->max_millivolts) &&
		    (millivolts[i] > d->max_millivolts)) {
			pr_warn("tegra_dvfs: voltage %d too high for dvfs on"
				" %s\n", millivolts[i], d->clk_name);
			return -EINVAL;
		}

		mv = millivolts[i];
		detach_mv = tegra_dvfs_rail_get_boot_level(d->dvfs_rail);
		if (!d->dvfs_rail->reg && (mv > detach_mv)) {
			pr_warn("%s: %s: voltage %d above boot limit %d\n",
				__func__, d->clk_name, mv, detach_mv);
			return -EINVAL;
		}

		detach_mv = tegra_dvfs_rail_get_disable_level(d->dvfs_rail);
		if (d->dvfs_rail->disabled && (mv > detach_mv)) {
			pr_warn("%s: %s: voltage %d above disable limit %d\n",
				__func__, d->clk_name, mv, detach_mv);
			return -EINVAL;
		}

		detach_mv = tegra_dvfs_rail_get_suspend_level(d->dvfs_rail);
		if (d->dvfs_rail->suspended && (mv > detach_mv)) {
			pr_warn("%s: %s: voltage %d above disable limit %d\n",
				__func__, d->clk_name, mv, detach_mv);
			return -EINVAL;
		}
		d->cur_millivolts = millivolts[i];
	}

	d->cur_rate = rate;

	d->dvfs_rail->rate_set = true;
	ret = dvfs_rail_update(d->dvfs_rail);
	if (ret)
		pr_err("Failed to set regulator %s for clock %s to %d mV\n",
			d->dvfs_rail->reg_id, d->clk_name, d->cur_millivolts);

	return ret;
}

int tegra_dvfs_alt_freqs_set(struct dvfs *d, unsigned long *alt_freqs)
{
	int ret = 0;

	mutex_lock(&dvfs_lock);

	if (d->alt_freqs != alt_freqs) {
		d->alt_freqs = alt_freqs;
		ret = __tegra_dvfs_set_rate(d, d->cur_rate);
	}

	mutex_unlock(&dvfs_lock);
	return ret;
}

static int predict_millivolts(struct clk *c, const int *millivolts,
			      unsigned long rate)
{
	int i;

	if (!millivolts)
		return -ENODEV;
	/*
	 * Predicted voltage can not be used across the switch to alternative
	 * frequency limits. For now, just fail the call for clock that has
	 * alternative limits initialized.
	 */
	if (c->dvfs->alt_freqs)
		return -ENOSYS;

	for (i = 0; i < c->dvfs->num_freqs; i++) {
		if (rate <= c->dvfs->freqs[i])
			break;
	}

	if (i == c->dvfs->num_freqs)
		return -EINVAL;

	return millivolts[i];
}

int tegra_dvfs_predict_millivolts(struct clk *c, unsigned long rate)
{
	const int *millivolts;

	if (!rate || !c->dvfs)
		return 0;

	millivolts = tegra_dvfs_is_dfll_range(c->dvfs, rate) ?
		c->dvfs->dfll_millivolts :
		tegra_dvfs_get_millivolts_pll(c->dvfs);
	return predict_millivolts(c, millivolts, rate);
}

int tegra_dvfs_predict_millivolts_pll(struct clk *c, unsigned long rate)
{
	const int *millivolts;

	if (!rate || !c->dvfs)
		return 0;

	millivolts = tegra_dvfs_get_millivolts_pll(c->dvfs);
	return predict_millivolts(c, millivolts, rate);
}

int tegra_dvfs_predict_millivolts_dfll(struct clk *c, unsigned long rate)
{
	const int *millivolts;

	if (!rate || !c->dvfs)
		return 0;

	millivolts = c->dvfs->dfll_millivolts;
	return predict_millivolts(c, millivolts, rate);
}

const int *tegra_dvfs_get_millivolts_pll(struct dvfs *d)
{
	if (d->therm_dvfs) {
		int therm_idx = d->dvfs_rail->therm_scale_idx;
		return d->millivolts + therm_idx * MAX_DVFS_FREQS;
	}
	return d->millivolts;
}

int tegra_dvfs_set_rate(struct clk *c, unsigned long rate)
{
	int ret;

	if (!c->dvfs)
		return -EINVAL;

	mutex_lock(&dvfs_lock);
	ret = __tegra_dvfs_set_rate(c->dvfs, rate);
	mutex_unlock(&dvfs_lock);

	return ret;
}
EXPORT_SYMBOL(tegra_dvfs_set_rate);

int tegra_dvfs_get_freqs(struct clk *c, unsigned long **freqs, int *num_freqs)
{
	if (!c->dvfs)
		return -ENOSYS;

	if (c->dvfs->alt_freqs)
		return -ENOSYS;

	*num_freqs = c->dvfs->num_freqs;
	*freqs = c->dvfs->freqs;

	return 0;
}
EXPORT_SYMBOL(tegra_dvfs_get_freqs);

#ifdef CONFIG_TEGRA_VDD_CORE_OVERRIDE
static DEFINE_MUTEX(rail_override_lock);

static int dvfs_override_core_voltage(int override_mv)
{
	int ret, floor, ceiling;
	struct dvfs_rail *rail = tegra_core_rail;

	if (!rail)
		return -ENOENT;

	if (rail->fixed_millivolts)
		return -ENOSYS;

	floor = rail->min_override_millivolts;
	ceiling = rail->nominal_millivolts;
	if (override_mv && ((override_mv < floor) || (override_mv > ceiling))) {
		pr_err("%s: override level %d outside the range [%d...%d]\n",
		       __func__, override_mv, floor, ceiling);
		return -EINVAL;
	}

	mutex_lock(&rail_override_lock);

	if (override_mv == rail->override_millivolts) {
		ret = 0;
		goto out;
	}

	if (override_mv) {
		ret = tegra_dvfs_core_cap_level_apply(override_mv);
		if (ret) {
			pr_err("%s: failed to set cap for override level %d\n",
			       __func__, override_mv);
			goto out;
		}
	}

	mutex_lock(&dvfs_lock);
	if (rail->disabled || rail->suspended) {
		pr_err("%s: cannot scale %s rail\n", __func__,
		       rail->disabled ? "disabled" : "suspended");
		ret = -EPERM;
		if (!override_mv) {
			mutex_unlock(&dvfs_lock);
			goto out;
		}
	} else {
		rail->override_millivolts = override_mv;
		ret = dvfs_rail_update(rail);
		if (ret) {
			pr_err("%s: failed to set override level %d\n",
			       __func__, override_mv);
			rail->override_millivolts = 0;
			dvfs_rail_update(rail);
		}
	}
	mutex_unlock(&dvfs_lock);

	if (!override_mv || ret)
		tegra_dvfs_core_cap_level_apply(0);
out:
	mutex_unlock(&rail_override_lock);
	return ret;
}
#else
static int dvfs_override_core_voltage(int override_mv)
{
	pr_err("%s: vdd core override is not supported\n", __func__);
	return -ENOSYS;
}
#endif

int tegra_dvfs_override_core_voltage(struct clk *c, int override_mv)
{
	if (!c->dvfs || !c->dvfs->can_override) {
		pr_err("%s: %s cannot override vdd core\n", __func__, c->name);
		return -EPERM;
	}
	return dvfs_override_core_voltage(override_mv);
}
EXPORT_SYMBOL(tegra_dvfs_override_core_voltage);

/* May only be called during clock init, does not take any locks on clock c. */
int __init tegra_enable_dvfs_on_clk(struct clk *c, struct dvfs *d)
{
	int i;

	if (c->dvfs) {
		pr_err("Error when enabling dvfs on %s for clock %s:\n",
			d->dvfs_rail->reg_id, c->name);
		pr_err("DVFS already enabled for %s\n",
			c->dvfs->dvfs_rail->reg_id);
		return -EINVAL;
	}

	for (i = 0; i < MAX_DVFS_FREQS; i++) {
		if (d->millivolts[i] == 0)
			break;

		d->freqs[i] *= d->freqs_mult;

		/* If final frequencies are 0, pad with previous frequency */
		if (d->freqs[i] == 0 && i > 1)
			d->freqs[i] = d->freqs[i - 1];
	}
	d->num_freqs = i;

	if (d->auto_dvfs) {
		c->auto_dvfs = true;
		clk_set_cansleep(c);
	}

	c->dvfs = d;

	/*
	 * Minimum core override level is determined as maximum voltage required
	 * for clocks outside shared buses (shared bus rates can be capped to
	 * safe levels when override limit is set)
	 */
	if (i && c->ops && !c->ops->shared_bus_update &&
	    !(c->flags & PERIPH_ON_CBUS) && !d->can_override) {
		int mv = tegra_dvfs_predict_millivolts(c, d->freqs[i-1]);
		if (d->dvfs_rail->min_override_millivolts < mv)
			d->dvfs_rail->min_override_millivolts = mv;
	}

	mutex_lock(&dvfs_lock);
	list_add_tail(&d->reg_node, &d->dvfs_rail->dvfs);
	mutex_unlock(&dvfs_lock);

	return 0;
}

static bool tegra_dvfs_all_rails_suspended(void)
{
	struct dvfs_rail *rail;
	bool all_suspended = true;

	list_for_each_entry(rail, &dvfs_rail_list, node)
		if (!rail->suspended && !rail->disabled)
			all_suspended = false;

	return all_suspended;
}

static bool tegra_dvfs_from_rails_suspended_or_solved(struct dvfs_rail *to)
{
	struct dvfs_relationship *rel;
	bool all_suspended = true;

	list_for_each_entry(rel, &to->relationships_from, from_node)
		if (!rel->from->suspended && !rel->from->disabled &&
			!rel->solved_at_nominal)
			all_suspended = false;

	return all_suspended;
}

static int tegra_dvfs_suspend_one(void)
{
	struct dvfs_rail *rail;
	int ret, mv;

	list_for_each_entry(rail, &dvfs_rail_list, node) {
		if (!rail->suspended && !rail->disabled &&
		    tegra_dvfs_from_rails_suspended_or_solved(rail)) {
			/* Safe, as pll mode rate is capped to fixed level */
			if (!rail->dfll_mode && rail->fixed_millivolts) {
				mv = rail->fixed_millivolts;
			} else {
				mv = tegra_dvfs_rail_get_suspend_level(rail);
				mv = dvfs_rail_apply_limits(rail, mv);
			}

			/* apply suspend limit only if it is above current mv */
			ret = -EPERM;
			if (mv >= rail->millivolts)
				ret = dvfs_rail_set_voltage(rail, mv);
			if (ret) {
				pr_err("tegra_dvfs: failed %s suspend at %d\n",
				       rail->reg_id, rail->millivolts);
				return ret;
			}

			rail->suspended = true;
			return 0;
		}
	}

	return -EINVAL;
}

static void tegra_dvfs_resume(void)
{
	struct dvfs_rail *rail;

	mutex_lock(&dvfs_lock);

	list_for_each_entry(rail, &dvfs_rail_list, node)
		rail->suspended = false;

	list_for_each_entry(rail, &dvfs_rail_list, node)
		dvfs_rail_update(rail);

	mutex_unlock(&dvfs_lock);
}

static int tegra_dvfs_suspend(void)
{
	int ret = 0;

	mutex_lock(&dvfs_lock);

	while (!tegra_dvfs_all_rails_suspended()) {
		ret = tegra_dvfs_suspend_one();
		if (ret)
			break;
	}

	mutex_unlock(&dvfs_lock);

	if (ret)
		tegra_dvfs_resume();

	return ret;
}

static int tegra_dvfs_pm_suspend(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	if (event == PM_SUSPEND_PREPARE) {
		if (tegra_dvfs_suspend())
			return NOTIFY_STOP;
		pr_info("tegra_dvfs: suspended\n");
	}
	return NOTIFY_OK;
};

static int tegra_dvfs_pm_resume(struct notifier_block *nb,
				unsigned long event, void *data)
{
	if (event == PM_POST_SUSPEND) {
		tegra_dvfs_resume();
		pr_info("tegra_dvfs: resumed\n");
	}
	return NOTIFY_OK;
};

static struct notifier_block tegra_dvfs_suspend_nb = {
	.notifier_call = tegra_dvfs_pm_suspend,
	.priority = -1,
};

static struct notifier_block tegra_dvfs_resume_nb = {
	.notifier_call = tegra_dvfs_pm_resume,
	.priority = 1,
};

static int tegra_dvfs_reboot_notify(struct notifier_block *nb,
				unsigned long event, void *data)
{
	switch (event) {
	case SYS_RESTART:
	case SYS_HALT:
	case SYS_POWER_OFF:
		tegra_dvfs_suspend();
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct notifier_block tegra_dvfs_reboot_nb = {
	.notifier_call = tegra_dvfs_reboot_notify,
};

/* must be called with dvfs lock held */
static void __tegra_dvfs_rail_disable(struct dvfs_rail *rail)
{
	int ret = -EPERM;
	int mv;

	/* don't set voltage in DFLL mode - won't work, but break stats */
	if (rail->dfll_mode) {
		rail->disabled = true;
		return;
	}

	/* Safe, as pll mode rate is capped to fixed level */
	if (!rail->dfll_mode && rail->fixed_millivolts) {
		mv = rail->fixed_millivolts;
	} else {
		mv = tegra_dvfs_rail_get_disable_level(rail);
		mv = dvfs_rail_apply_limits(rail, mv);
	}

	/* apply detach mode limit provided it is above current volatge */
	if (mv >= rail->millivolts)
		ret = dvfs_rail_set_voltage(rail, mv);
	if (ret) {
		pr_err("tegra_dvfs: failed to disable %s at %d\n",
		       rail->reg_id, rail->millivolts);
		return;
	}
	rail->disabled = true;
}

/* must be called with dvfs lock held */
static void __tegra_dvfs_rail_enable(struct dvfs_rail *rail)
{
	rail->disabled = false;
	dvfs_rail_update(rail);
}

void tegra_dvfs_rail_enable(struct dvfs_rail *rail)
{
	if (!rail)
		return;

	mutex_lock(&rail_disable_lock);

	if (rail->disabled) {
		mutex_lock(&dvfs_lock);
		__tegra_dvfs_rail_enable(rail);
		mutex_unlock(&dvfs_lock);

		tegra_dvfs_rail_post_enable(rail);
	}
	mutex_unlock(&rail_disable_lock);
}

void tegra_dvfs_rail_disable(struct dvfs_rail *rail)
{
	if (!rail)
		return;

	mutex_lock(&rail_disable_lock);
	if (rail->disabled)
		goto out;

	/* rail disable will set it to nominal voltage underneath clock
	   framework - need to re-configure clock rates that are not safe
	   at nominal (yes, unsafe at nominal is ugly, but possible). Rate
	   change must be done outside of dvfs lock. */
	if (tegra_dvfs_rail_disable_prepare(rail)) {
		pr_info("dvfs: failed to prepare regulator %s to disable\n",
			rail->reg_id);
		goto out;
	}

	mutex_lock(&dvfs_lock);
	__tegra_dvfs_rail_disable(rail);
	mutex_unlock(&dvfs_lock);
out:
	mutex_unlock(&rail_disable_lock);
}

int tegra_dvfs_rail_disable_by_name(const char *reg_id)
{
	struct dvfs_rail *rail = tegra_dvfs_get_rail_by_name(reg_id);
	if (!rail)
		return -EINVAL;

	tegra_dvfs_rail_disable(rail);
	return 0;
}

struct dvfs_rail *tegra_dvfs_get_rail_by_name(const char *reg_id)
{
	struct dvfs_rail *rail;

	mutex_lock(&dvfs_lock);
	list_for_each_entry(rail, &dvfs_rail_list, node) {
		if (!strcmp(reg_id, rail->reg_id)) {
			mutex_unlock(&dvfs_lock);
			return rail;
		}
	}
	mutex_unlock(&dvfs_lock);
	return NULL;
}

int tegra_dvfs_rail_power_up(struct dvfs_rail *rail)
{
	int ret = -ENOENT;

	if (!rail || !rail->in_band_pm)
		return -ENOSYS;

	mutex_lock(&dvfs_lock);
	if (rail->reg) {
		ret = regulator_enable(rail->reg);
		if (!ret && !timekeeping_suspended)
			tegra_dvfs_rail_on(rail, ktime_get());
	}
	mutex_unlock(&dvfs_lock);
	return ret;
}

int tegra_dvfs_rail_power_down(struct dvfs_rail *rail)
{
	int ret = -ENOENT;

	if (!rail || !rail->in_band_pm)
		return -ENOSYS;

	mutex_lock(&dvfs_lock);
	if (rail->reg) {
		ret = regulator_disable(rail->reg);
		if (!ret && !timekeeping_suspended)
			tegra_dvfs_rail_off(rail, ktime_get());
	}
	mutex_unlock(&dvfs_lock);
	return ret;
}

bool tegra_dvfs_is_rail_up(struct dvfs_rail *rail)
{
	bool ret = false;

	if (!rail)
		return false;

	if (!rail->in_band_pm)
		return true;

	mutex_lock(&dvfs_lock);
	if (rail->reg)
		ret = regulator_is_enabled(rail->reg) > 0;
	mutex_unlock(&dvfs_lock);
	return ret;
}

bool tegra_dvfs_rail_updating(struct clk *clk)
{
	return (!clk ? false :
		(!clk->dvfs ? false :
		 (!clk->dvfs->dvfs_rail ? false :
		  (clk->dvfs->dvfs_rail->updating ||
		   clk->dvfs->dvfs_rail->dfll_mode_updating))));
}

#ifdef CONFIG_OF
int __init of_tegra_dvfs_init(const struct of_device_id *matches)
{
	int ret;
	struct device_node *np;

	for_each_matching_node(np, matches) {
		const struct of_device_id *match = of_match_node(matches, np);
		of_tegra_dvfs_init_cb_t dvfs_init_cb = match->data;
		ret = dvfs_init_cb(np);
		if (ret) {
			pr_err("dt: Failed to read %s tables from DT\n",
							match->compatible);
			return ret;
		}
	}
	return 0;
}
#endif
int tegra_dvfs_dfll_mode_set(struct dvfs *d, unsigned long rate)
{
	mutex_lock(&dvfs_lock);
	if (!d->dvfs_rail->dfll_mode) {
		d->dvfs_rail->dfll_mode = true;
		__tegra_dvfs_set_rate(d, rate);
	}
	mutex_unlock(&dvfs_lock);
	return 0;
}

int tegra_dvfs_dfll_mode_clear(struct dvfs *d, unsigned long rate)
{
	int ret = 0;

	mutex_lock(&dvfs_lock);
	if (d->dvfs_rail->dfll_mode) {
		d->dvfs_rail->dfll_mode = false;
		/* avoid false detection of matching target (voltage in dfll
		   mode is fluctuating, and recorded level is just estimate) */
		d->dvfs_rail->millivolts--;
		if (d->dvfs_rail->disabled) {
			d->dvfs_rail->disabled = false;
			__tegra_dvfs_rail_disable(d->dvfs_rail);
		}
		ret = __tegra_dvfs_set_rate(d, rate);
	}
	mutex_unlock(&dvfs_lock);
	return ret;
}

struct tegra_cooling_device *tegra_dvfs_get_cpu_vmax_cdev(void)
{
	if (tegra_cpu_rail)
		return tegra_cpu_rail->vmax_cdev;
	return NULL;
}

struct tegra_cooling_device *tegra_dvfs_get_cpu_vmin_cdev(void)
{
	if (tegra_cpu_rail)
		return tegra_cpu_rail->vmin_cdev;
	return NULL;
}

struct tegra_cooling_device *tegra_dvfs_get_core_vmin_cdev(void)
{
	if (tegra_core_rail)
		return tegra_core_rail->vmin_cdev;
	return NULL;
}

struct tegra_cooling_device *tegra_dvfs_get_gpu_vmin_cdev(void)
{
	if (tegra_gpu_rail)
		return tegra_gpu_rail->vmin_cdev;
	return NULL;
}

struct tegra_cooling_device *tegra_dvfs_get_gpu_vts_cdev(void)
{
	if (tegra_gpu_rail)
		return tegra_gpu_rail->vts_cdev;
	return NULL;
}

static void make_safe_thermal_dvfs_one(struct dvfs *d,
				  struct tegra_cooling_device *cdev)
{
	int i, j, mv;

	/* Make 1st row (therm_idx = 0) voltages max across thermal ranges */
	for (i = 0; i < d->num_freqs; i++) {
		for (j = 1; j <= cdev->trip_temperatures_num; j++) {
			mv = *(d->millivolts + j * MAX_DVFS_FREQS + i);
			if (d->millivolts[i] < mv)
				((int *)d->millivolts)[i] = mv;
		}
	}
}

static void make_safe_thermal_dvfs(struct dvfs_rail *rail)
{
	struct dvfs *d;

	mutex_lock(&dvfs_lock);
	list_for_each_entry(d, &rail->dvfs, reg_node) {
		if (d->therm_dvfs)
			make_safe_thermal_dvfs_one(d, rail->vts_cdev);
	}
	mutex_unlock(&dvfs_lock);
}

#ifdef CONFIG_THERMAL
/* Cooling device limits minimum rail voltage at cold temperature in pll mode */
static int tegra_dvfs_rail_get_vmin_cdev_max_state(
	struct thermal_cooling_device *cdev, unsigned long *max_state)
{
	struct dvfs_rail *rail = (struct dvfs_rail *)cdev->devdata;
	*max_state = rail->vmin_cdev->trip_temperatures_num;
	return 0;
}

static int tegra_dvfs_rail_get_vmin_cdev_cur_state(
	struct thermal_cooling_device *cdev, unsigned long *cur_state)
{
	struct dvfs_rail *rail = (struct dvfs_rail *)cdev->devdata;
	*cur_state = rail->therm_floor_idx;
	return 0;
}

static int tegra_dvfs_rail_set_vmin_cdev_state(
	struct thermal_cooling_device *cdev, unsigned long cur_state)
{
	struct dvfs_rail *rail = (struct dvfs_rail *)cdev->devdata;

	mutex_lock(&dvfs_lock);
	if (rail->therm_floor_idx != cur_state) {
		rail->therm_floor_idx = cur_state;
		dvfs_rail_update(rail);
	}
	mutex_unlock(&dvfs_lock);
	return 0;
}

static struct thermal_cooling_device_ops tegra_dvfs_vmin_cooling_ops = {
	.get_max_state = tegra_dvfs_rail_get_vmin_cdev_max_state,
	.get_cur_state = tegra_dvfs_rail_get_vmin_cdev_cur_state,
	.set_cur_state = tegra_dvfs_rail_set_vmin_cdev_state,
};

static void tegra_dvfs_rail_register_vmin_cdev(struct dvfs_rail *rail)
{
	if (!rail->vmin_cdev)
		return;

	/* just report error - initialized for cold temperature, anyway */
	if (IS_ERR_OR_NULL(thermal_cooling_device_register(
		rail->vmin_cdev->cdev_type, (void *)rail,
		&tegra_dvfs_vmin_cooling_ops)))
		pr_err("tegra cooling device %s failed to register\n",
		       rail->vmin_cdev->cdev_type);
}

/* Cooling device to scale voltage with temperature in pll mode */
static int tegra_dvfs_rail_get_vts_cdev_max_state(
	struct thermal_cooling_device *cdev, unsigned long *max_state)
{
	struct dvfs_rail *rail = (struct dvfs_rail *)cdev->devdata;
	*max_state = rail->vts_cdev->trip_temperatures_num;
	return 0;
}

static int tegra_dvfs_rail_get_vts_cdev_cur_state(
	struct thermal_cooling_device *cdev, unsigned long *cur_state)
{
	struct dvfs_rail *rail = (struct dvfs_rail *)cdev->devdata;
	*cur_state = rail->therm_scale_idx;
	return 0;
}

static int tegra_dvfs_rail_set_vts_cdev_state(
	struct thermal_cooling_device *cdev, unsigned long cur_state)
{
	struct dvfs_rail *rail = (struct dvfs_rail *)cdev->devdata;
	struct dvfs *d;

	mutex_lock(&dvfs_lock);
	if (rail->therm_scale_idx != cur_state) {
		rail->therm_scale_idx = cur_state;
		list_for_each_entry(d, &rail->dvfs, reg_node) {
			if (d->therm_dvfs)
				__tegra_dvfs_set_rate(d, d->cur_rate);
		}
	}
	mutex_unlock(&dvfs_lock);
	return 0;
}

static struct thermal_cooling_device_ops tegra_dvfs_vts_cooling_ops = {
	.get_max_state = tegra_dvfs_rail_get_vts_cdev_max_state,
	.get_cur_state = tegra_dvfs_rail_get_vts_cdev_cur_state,
	.set_cur_state = tegra_dvfs_rail_set_vts_cdev_state,
};

static void tegra_dvfs_rail_register_vts_cdev(struct dvfs_rail *rail)
{
	struct thermal_cooling_device *dev;

	if (!rail->vts_cdev)
		return;

	dev = thermal_cooling_device_register(rail->vts_cdev->cdev_type,
		(void *)rail, &tegra_dvfs_vts_cooling_ops);
	/* report error & set max limits across thermal ranges as safe dvfs */
	if (IS_ERR_OR_NULL(dev) || list_empty(&dev->thermal_instances)) {
		pr_err("tegra cooling device %s failed to register\n",
		       rail->vts_cdev->cdev_type);
		make_safe_thermal_dvfs(rail);
	}
}

#else
#define tegra_dvfs_rail_register_vmin_cdev(rail)
static inline void tegra_dvfs_rail_register_vts_cdev(struct dvfs_rail *rail)
{
	make_safe_thermal_dvfs(rail);
}
#endif

/*
 * Validate rail thermal profile, and get its size. Valid profile:
 * - voltage limits are descending with temperature increasing
 * - the lowest limit is above rail minimum voltage in pll and
 *   in dfll mode (if applicable)
 * - the highest limit is below rail nominal voltage
 */
static int __init get_thermal_profile_size(
	int *trips_table, int *limits_table,
	struct dvfs_rail *rail, struct dvfs_dfll_data *d)
{
	int i, min_mv;

	for (i = 0; i < MAX_THERMAL_LIMITS - 1; i++) {
		if (!limits_table[i+1])
			break;

		if ((trips_table[i] >= trips_table[i+1]) ||
		    (limits_table[i] < limits_table[i+1])) {
			pr_warn("%s: not ordered profile\n", rail->reg_id);
			return -EINVAL;
		}
	}

	min_mv = max(rail->min_millivolts, d ? d->min_millivolts : 0);
	if (limits_table[i] < min_mv) {
		pr_warn("%s: thermal profile below Vmin\n", rail->reg_id);
		return -EINVAL;
	}

	if (limits_table[0] > rail->nominal_millivolts) {
		pr_warn("%s: thermal profile above Vmax\n", rail->reg_id);
		return -EINVAL;
	}
	return i + 1;
}

void __init tegra_dvfs_rail_init_vmax_thermal_profile(
	int *therm_trips_table, int *therm_caps_table,
	struct dvfs_rail *rail, struct dvfs_dfll_data *d)
{
	int i = get_thermal_profile_size(therm_trips_table,
					 therm_caps_table, rail, d);
	if (i <= 0) {
		rail->vmax_cdev = NULL;
		WARN(1, "%s: invalid Vmax thermal profile\n", rail->reg_id);
		return;
	}

	/* Install validated thermal caps */
	rail->therm_mv_caps = therm_caps_table;
	rail->therm_mv_caps_num = i;

	/* Setup trip-points if applicable */
	if (rail->vmax_cdev) {
		rail->vmax_cdev->trip_temperatures_num = i;
		rail->vmax_cdev->trip_temperatures = therm_trips_table;
	}
}

void __init tegra_dvfs_rail_init_vmin_thermal_profile(
	int *therm_trips_table, int *therm_floors_table,
	struct dvfs_rail *rail, struct dvfs_dfll_data *d)
{
	int i = get_thermal_profile_size(therm_trips_table,
					 therm_floors_table, rail, d);
	if (i <= 0) {
		rail->vmin_cdev = NULL;
		WARN(1, "%s: invalid Vmin thermal profile\n", rail->reg_id);
		return;
	}

	/* Install validated thermal floors */
	rail->therm_mv_floors = therm_floors_table;
	rail->therm_mv_floors_num = i;

	/* Setup trip-points if applicable */
	if (rail->vmin_cdev) {
		rail->vmin_cdev->trip_temperatures_num = i;
		rail->vmin_cdev->trip_temperatures = therm_trips_table;
	}
}

/*
 * Validate thermal dvfs settings:
 * - trip-points are montonically increasing
 * - voltages in any temperature range are montonically increasing with
 *   frequency (can go up/down across ranges at iso frequency)
 * - voltage for any frequency/thermal range combination must be within
 *   rail minimum/maximum limits
 */
int __init tegra_dvfs_rail_init_thermal_dvfs_trips(
	int *therm_trips_table, struct dvfs_rail *rail)
{
	int i;

	if (!rail->vts_cdev) {
		WARN(1, "%s: missing thermal dvfs cooling device\n",
		     rail->reg_id);
		return -ENOENT;
	}

	for (i = 0; i < MAX_THERMAL_LIMITS - 1; i++) {
		if (therm_trips_table[i] >= therm_trips_table[i+1])
			break;
	}

	rail->vts_cdev->trip_temperatures_num = i + 1;
	rail->vts_cdev->trip_temperatures = therm_trips_table;
	return 0;
}

int __init tegra_dvfs_init_thermal_dvfs_voltages(
	int *therm_voltages, int freqs_num, int ranges_num, struct dvfs *d)
{
	int *millivolts;
	int freq_idx, therm_idx;

	for (therm_idx = 0; therm_idx < ranges_num; therm_idx++) {
		millivolts = therm_voltages + therm_idx * MAX_DVFS_FREQS;
		for (freq_idx = 0; freq_idx < freqs_num; freq_idx++) {
			int mv = millivolts[freq_idx];
			if ((mv > d->dvfs_rail->max_millivolts) ||
			    (mv < d->dvfs_rail->min_millivolts) ||
			    (freq_idx && (mv < millivolts[freq_idx - 1]))) {
				WARN(1, "%s: invalid thermal dvfs entry %d(%d, %d)\n",
				     d->clk_name, mv, freq_idx, therm_idx);
				return -EINVAL;
			}
		}
	}

	d->millivolts = therm_voltages;
	d->therm_dvfs = true;
	return 0;
}

/* Directly set cold temperature limit in dfll mode */
int tegra_dvfs_rail_dfll_mode_set_cold(struct dvfs_rail *rail)
{
	int ret = 0;

	/* No thermal floors - nothing to do */
	if (!rail || !rail->therm_mv_floors)
		return ret;

	/*
	 * Since cooling thresholds are the same in pll and dfll modes, pll mode
	 * thermal index can be used to decide if cold limit should be set in
	 * dfll mode.
	 */
	mutex_lock(&dvfs_lock);
	if (rail->dfll_mode &&
	    (rail->therm_floor_idx < rail->therm_mv_floors_num)) {
			int mv = rail->therm_mv_floors[rail->therm_floor_idx];
			ret = dvfs_rail_set_voltage_reg(rail, mv);
	}
	mutex_unlock(&dvfs_lock);

	return ret;
}

/*
 * Iterate through all the dvfs regulators, finding the regulator exported
 * by the regulator api for each one.  Must be called in late init, after
 * all the regulator api's regulators are initialized.
 */
int __init tegra_dvfs_late_init(void)
{
	bool connected = true;
	struct dvfs_rail *rail;

	mutex_lock(&dvfs_lock);

	list_for_each_entry(rail, &dvfs_rail_list, node)
		if (dvfs_rail_connect_to_regulator(rail))
			connected = false;

	list_for_each_entry(rail, &dvfs_rail_list, node)
		if (connected)
			dvfs_rail_update(rail);
		else
			__tegra_dvfs_rail_disable(rail);

	mutex_unlock(&dvfs_lock);

	if (!connected && tegra_platform_is_silicon()) {
		pr_warn("tegra_dvfs: DVFS regulators connection failed\n"
			"            !!!! voltage scaling is disabled !!!!\n");
		return -ENODEV;
	}

	register_pm_notifier(&tegra_dvfs_suspend_nb);
	register_pm_notifier(&tegra_dvfs_resume_nb);
	register_reboot_notifier(&tegra_dvfs_reboot_nb);

	list_for_each_entry(rail, &dvfs_rail_list, node) {
			tegra_dvfs_rail_register_vmin_cdev(rail);
			tegra_dvfs_rail_register_vts_cdev(rail);
	}

	return 0;
}

static int rail_stats_save_to_buf(char *buf, int len)
{
	int i;
	struct dvfs_rail *rail;
	char *str = buf;
	char *end = buf + len;

	str += scnprintf(str, end - str, "%-12s %-10s\n", "millivolts", "time");

	mutex_lock(&dvfs_lock);

	list_for_each_entry(rail, &dvfs_rail_list, node) {
		str += scnprintf(str, end - str, "%s (bin: %d.%dmV)\n",
			   rail->reg_id,
			   rail->stats.bin_uV / 1000,
			   (rail->stats.bin_uV / 10) % 100);

		dvfs_rail_stats_update(rail, -1, ktime_get());

		str += scnprintf(str, end - str, "%-12d %-10llu\n", 0,
			cputime64_to_clock_t(msecs_to_jiffies(
				ktime_to_ms(rail->stats.time_at_mv[0]))));

		for (i = 1; i <= DVFS_RAIL_STATS_TOP_BIN; i++) {
			ktime_t ktime_zero = ktime_set(0, 0);
			if (ktime_equal(rail->stats.time_at_mv[i], ktime_zero))
				continue;
			str += scnprintf(str, end - str, "%-12d %-10llu\n",
				rail->min_millivolts +
				(i - 1) * rail->stats.bin_uV / 1000,
				cputime64_to_clock_t(msecs_to_jiffies(
					ktime_to_ms(rail->stats.time_at_mv[i])))
			);
		}
	}
	mutex_unlock(&dvfs_lock);
	return str - buf;
}

#ifdef CONFIG_DEBUG_FS
static int dvfs_tree_sort_cmp(void *p, struct list_head *a, struct list_head *b)
{
	struct dvfs *da = list_entry(a, struct dvfs, reg_node);
	struct dvfs *db = list_entry(b, struct dvfs, reg_node);
	int ret;

	ret = strcmp(da->dvfs_rail->reg_id, db->dvfs_rail->reg_id);
	if (ret != 0)
		return ret;

	if (da->cur_millivolts < db->cur_millivolts)
		return 1;
	if (da->cur_millivolts > db->cur_millivolts)
		return -1;

	return strcmp(da->clk_name, db->clk_name);
}

static int dvfs_tree_show(struct seq_file *s, void *data)
{
	struct dvfs *d;
	struct dvfs_rail *rail;
	struct dvfs_relationship *rel;

	seq_printf(s, "   clock      rate       mV\n");
	seq_printf(s, "--------------------------------\n");

	mutex_lock(&dvfs_lock);

	list_for_each_entry(rail, &dvfs_rail_list, node) {
		int thermal_mv_floor = 0;

		seq_printf(s, "%s %d mV%s:\n", rail->reg_id,
			   rail->stats.off ? 0 : rail->millivolts,
			   rail->dfll_mode ? " dfll mode" :
				rail->disabled ? " disabled" : "");
		list_for_each_entry(rel, &rail->relationships_from, from_node) {
			seq_printf(s, "   %-10s %-7d mV %-4d mV\n",
				rel->from->reg_id, rel->from->millivolts,
				dvfs_solve_relationship(rel));
		}
		seq_printf(s, "   offset     %-7d mV\n", rail->dbg_mv_offs);

		if (rail->therm_mv_floors) {
			int i = rail->therm_floor_idx;
			if (i < rail->therm_mv_floors_num)
				thermal_mv_floor = rail->therm_mv_floors[i];
		}
		seq_printf(s, "   thermal    %-7d mV\n", thermal_mv_floor);

		if (rail == tegra_core_rail) {
			seq_printf(s, "   override   %-7d mV [%-4d...%-4d]\n",
				   rail->override_millivolts,
				   rail->min_override_millivolts,
				   rail->nominal_millivolts);
		}

		list_sort(NULL, &rail->dvfs, dvfs_tree_sort_cmp);

		list_for_each_entry(d, &rail->dvfs, reg_node) {
			seq_printf(s, "   %-10s %-10lu %-4d mV\n", d->clk_name,
				d->cur_rate, d->cur_millivolts);
		}
	}

	mutex_unlock(&dvfs_lock);

	return 0;
}

static int dvfs_tree_open(struct inode *inode, struct file *file)
{
	return single_open(file, dvfs_tree_show, inode->i_private);
}

static const struct file_operations dvfs_tree_fops = {
	.open		= dvfs_tree_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int rail_stats_show(struct seq_file *s, void *data)
{
	char *buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	int size = 0;

	if (!buf)
		return -ENOMEM;

	size = rail_stats_save_to_buf(buf, PAGE_SIZE);
	seq_write(s, buf, size);
	kfree(buf);
	return 0;
}

static int rail_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, rail_stats_show, inode->i_private);
}

static const struct file_operations rail_stats_fops = {
	.open		= rail_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int rail_offs_set(struct dvfs_rail *rail, int offs)
{
	if (rail) {
		mutex_lock(&dvfs_lock);
		rail->dbg_mv_offs = offs;
		dvfs_rail_update(rail);
		mutex_unlock(&dvfs_lock);
		return 0;
	}
	return -ENOENT;
}

static int cpu_offs_get(void *data, u64 *val)
{
	if (tegra_cpu_rail) {
		*val = (u64)tegra_cpu_rail->dbg_mv_offs;
		return 0;
	}
	*val = 0;
	return -ENOENT;
}
static int cpu_offs_set(void *data, u64 val)
{
	return rail_offs_set(tegra_cpu_rail, (int)val);
}
DEFINE_SIMPLE_ATTRIBUTE(cpu_offs_fops, cpu_offs_get, cpu_offs_set, "%lld\n");

static int gpu_offs_get(void *data, u64 *val)
{
	if (tegra_gpu_rail) {
		*val = (u64)tegra_gpu_rail->dbg_mv_offs;
		return 0;
	}
	*val = 0;
	return -ENOENT;
}
static int gpu_offs_set(void *data, u64 val)
{
	return rail_offs_set(tegra_gpu_rail, (int)val);
}
DEFINE_SIMPLE_ATTRIBUTE(gpu_offs_fops, gpu_offs_get, gpu_offs_set, "%lld\n");

static int core_offs_get(void *data, u64 *val)
{
	if (tegra_core_rail) {
		*val = (u64)tegra_core_rail->dbg_mv_offs;
		return 0;
	}
	*val = 0;
	return -ENOENT;
}
static int core_offs_set(void *data, u64 val)
{
	return rail_offs_set(tegra_core_rail, (int)val);
}
DEFINE_SIMPLE_ATTRIBUTE(core_offs_fops, core_offs_get, core_offs_set, "%lld\n");

static int core_override_get(void *data, u64 *val)
{
	if (tegra_core_rail) {
		*val = (u64)tegra_core_rail->override_millivolts;
		return 0;
	}
	*val = 0;
	return -ENOENT;
}
static int core_override_set(void *data, u64 val)
{
	return dvfs_override_core_voltage((int)val);
}
DEFINE_SIMPLE_ATTRIBUTE(core_override_fops,
			core_override_get, core_override_set, "%llu\n");

static int gpu_dvfs_t_show(struct seq_file *s, void *data)
{
	int i, j;
	int num_ranges = 1;
	int *trips = NULL;
	struct dvfs *d;
	struct dvfs_rail *rail = tegra_gpu_rail;

	if (!tegra_gpu_rail) {
		seq_printf(s, "Only supported for T124 or higher\n");
		return -ENOSYS;
	}

	mutex_lock(&dvfs_lock);

	d = list_first_entry(&rail->dvfs, struct dvfs, reg_node);
	if (rail->vts_cdev && d->therm_dvfs) {
		num_ranges = rail->vts_cdev->trip_temperatures_num + 1;
		trips = rail->vts_cdev->trip_temperatures;
	}

	seq_printf(s, "%-11s", "T(C)\\F(kHz)");
	for (i = 0; i < d->num_freqs; i++) {
		unsigned int f = d->freqs[i]/100;
		seq_printf(s, " %7u", f);
	}
	seq_printf(s, "\n");

	for (j = 0; j < num_ranges; j++) {
		seq_printf(s, "%s", j == rail->therm_scale_idx ? ">" : " ");

		if (!trips || (num_ranges == 1))
			seq_printf(s, "%4s..%-4s", "", "");
		else if (j == 0)
			seq_printf(s, "%4s..%-4d", "", trips[j]);
		else if (j == num_ranges - 1)
			seq_printf(s, "%4d..%-4s", trips[j], "");
		else
			seq_printf(s, "%4d..%-4d", trips[j-1], trips[j]);

		for (i = 0; i < d->num_freqs; i++) {
			int mv = *(d->millivolts + j * MAX_DVFS_FREQS + i);
			seq_printf(s, " %7d", mv);
		}
		seq_printf(s, " mV\n");
	}

	mutex_unlock(&dvfs_lock);

	return 0;
}

static int gpu_dvfs_t_open(struct inode *inode, struct file *file)
{
	return single_open(file, gpu_dvfs_t_show, NULL);
}

static const struct file_operations gpu_dvfs_t_fops = {
	.open           = gpu_dvfs_t_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static int dvfs_table_show(struct seq_file *s, void *data)
{
	int i;
	struct dvfs *d;
	struct dvfs_rail *rail;
	const int *v_pll, *last_v_pll = NULL;
	const int *v_dfll, *last_v_dfll = NULL;

	seq_printf(s, "DVFS tables: units mV/MHz\n");

	mutex_lock(&dvfs_lock);

	list_for_each_entry(rail, &dvfs_rail_list, node) {
		list_for_each_entry(d, &rail->dvfs, reg_node) {
			bool mv_done = false;
			v_pll = tegra_dvfs_get_millivolts_pll(d);
			v_dfll = d->dfll_millivolts;

			if (v_pll && (last_v_pll != v_pll)) {
				if (!mv_done) {
					seq_printf(s, "\n");
					mv_done = true;
				}
				last_v_pll = v_pll;
				seq_printf(s, "%-16s", rail->reg_id);
				for (i = 0; i < d->num_freqs; i++)
					seq_printf(s, "%7d", v_pll[i]);
				seq_printf(s, "\n");
			}

			if (v_dfll && (last_v_dfll != v_dfll)) {
				if (!mv_done) {
					seq_printf(s, "\n");
					mv_done = true;
				}
				last_v_dfll = v_dfll;
				seq_printf(s, "%-8s (dfll) ", rail->reg_id);
				for (i = 0; i < d->num_freqs; i++)
					seq_printf(s, "%7d", v_dfll[i]);
				seq_printf(s, "\n");
			}

			seq_printf(s, "%-16s", d->clk_name);
			for (i = 0; i < d->num_freqs; i++) {
				unsigned int f = d->freqs[i]/100000;
				seq_printf(s, " %4u.%u", f/10, f%10);
			}
			seq_printf(s, "\n");
		}
	}

	mutex_unlock(&dvfs_lock);

	return 0;
}

static int dvfs_table_open(struct inode *inode, struct file *file)
{
	return single_open(file, dvfs_table_show, inode->i_private);
}

static const struct file_operations dvfs_table_fops = {
	.open		= dvfs_table_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

int __init dvfs_debugfs_init(struct dentry *clk_debugfs_root)
{
	struct dentry *d;

	d = debugfs_create_file("dvfs", S_IRUGO, clk_debugfs_root, NULL,
		&dvfs_tree_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("rails", S_IRUGO, clk_debugfs_root, NULL,
		&rail_stats_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("vdd_cpu_offs", S_IRUGO | S_IWUSR,
		clk_debugfs_root, NULL, &cpu_offs_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("vdd_gpu_offs", S_IRUGO | S_IWUSR,
		clk_debugfs_root, NULL, &gpu_offs_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("vdd_core_offs", S_IRUGO | S_IWUSR,
		clk_debugfs_root, NULL, &core_offs_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("vdd_core_override", S_IRUGO | S_IWUSR,
		clk_debugfs_root, NULL, &core_override_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("gpu_dvfs_t", S_IRUGO | S_IWUSR,
		clk_debugfs_root, NULL, &gpu_dvfs_t_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("dvfs_table", S_IRUGO, clk_debugfs_root, NULL,
		&dvfs_table_fops);
	if (!d)
		return -ENOMEM;

	return 0;
}

#endif

#ifdef CONFIG_PM
static ssize_t tegra_rail_stats_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return rail_stats_save_to_buf(buf, PAGE_SIZE);
}

static struct kobj_attribute rail_stats_attr =
		__ATTR_RO(tegra_rail_stats);

static int __init tegra_dvfs_sysfs_stats_init(void)
{
	int error;
	error = sysfs_create_file(power_kobj, &rail_stats_attr.attr);
	return 0;
}
late_initcall(tegra_dvfs_sysfs_stats_init);
#endif
