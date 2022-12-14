/*
 * arch/arch/mach-tegra/timer.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * Copyright (C) 2010-2013 NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/init.h>
#include <linux/err.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/syscore_ops.h>
#include <linux/of.h>

#include <asm/mach/time.h>
#include <asm/delay.h>
#ifndef CONFIG_ARM64
#include <asm/sched_clock.h>
#include <asm/localtimer.h>
#endif

#include "board.h"
#include "clock.h"
#include "iomap.h"
#include "timer.h"
#include "fuse.h"

static void __iomem *timer_reg_base = IO_ADDRESS(TEGRA_TMR1_BASE);
static void __iomem *rtc_base = IO_ADDRESS(TEGRA_RTC_BASE);

static u64 persistent_ms, last_persistent_ms;
static struct timespec persistent_ts;

static u32 usec_config;

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
static u32 system_timer = (TEGRA_TMR3_BASE - TEGRA_TMR1_BASE);
#else
static u32 system_timer = 0;
#endif

#if !defined(CONFIG_ARM_ARCH_TIMER) && !defined(CONFIG_HAVE_ARM_TWD)
#ifndef CONFIG_ARCH_TEGRA_2x_SOC

#define TIMER3_OFFSET (TEGRA_TMR3_BASE-TEGRA_TMR1_BASE)
#define TIMER4_OFFSET (TEGRA_TMR4_BASE-TEGRA_TMR1_BASE)
#define TIMER5_OFFSET (TEGRA_TMR5_BASE-TEGRA_TMR1_BASE)
#define TIMER6_OFFSET (TEGRA_TMR6_BASE-TEGRA_TMR1_BASE)

struct tegra_clock_event_device {
	struct clock_event_device *evt;
	char name[15];
};

static u32 cpu_local_timers[] = {
	TIMER3_OFFSET,
#ifdef CONFIG_SMP
	TIMER4_OFFSET,
	TIMER5_OFFSET,
	TIMER6_OFFSET,
#endif
};

static irqreturn_t tegra_cputimer_interrupt(int irq, void *dev_id)
{
	struct tegra_clock_event_device *clkevt = dev_id;
	struct clock_event_device *evt = clkevt->evt;
	int base;
	unsigned int cpu;

	cpu = cpumask_first(evt->cpumask);
	base = cpu_local_timers[cpu];
	timer_writel(1<<30, base + TIMER_PCR);
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static int tegra_cputimer_set_next_event(unsigned long cycles,
					 struct clock_event_device *evt)
{
	u32 reg;
	int base;
	unsigned int cpu;

	cpu = cpumask_first(evt->cpumask);
	base = cpu_local_timers[cpu];
	reg = 0x80000000 | ((cycles > 1) ? (cycles-1) : 0);
	timer_writel(reg, base + TIMER_PTV);

	return 0;
}

static void tegra_cputimer_set_mode(enum clock_event_mode mode,
				    struct clock_event_device *evt)
{
	u32 reg;
	int base;
	unsigned int cpu;

	cpu = cpumask_first(evt->cpumask);
	base = cpu_local_timers[cpu];
	timer_writel(0, base + TIMER_PTV);

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		reg = 0xC0000000 | ((1000000/HZ)-1);
		timer_writel(reg, base + TIMER_PTV);
		break;
	case CLOCK_EVT_MODE_ONESHOT:
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	case CLOCK_EVT_MODE_RESUME:
		break;
	}
}

static struct clock_event_device tegra_cputimer_clockevent = {
	.rating		= 450,
	.features	= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC,
	.set_next_event	= tegra_cputimer_set_next_event,
	.set_mode	= tegra_cputimer_set_mode,
};

#define CPU_TIMER_IRQ_ACTION(cpu, irqnum) {                     \
	.name		= "local_tmr_cpu" __stringify(cpu),   \
	.flags		= IRQF_TIMER | IRQF_IRQPOLL,		\
	.handler	= tegra_cputimer_interrupt,              \
	.irq		= irqnum }

static struct irqaction tegra_cputimer_irq[] = {
	CPU_TIMER_IRQ_ACTION(0, INT_TMR3),
#ifdef CONFIG_SMP
	CPU_TIMER_IRQ_ACTION(1, INT_TMR4),
	CPU_TIMER_IRQ_ACTION(2, INT_TMR5),
	CPU_TIMER_IRQ_ACTION(3, INT_TMR6),
#endif
};
#endif /* CONFIG_ARCH_TEGRA_2x_SOC */
#endif /* CONFIG_ARM_ARCH_TIMER && CONFIG_HAVE_ARM_TWD */

static int tegra_timer_set_next_event(unsigned long cycles,
					 struct clock_event_device *evt)
{
	u32 reg;

	reg = 0x80000000 | ((cycles > 1) ? (cycles-1) : 0);
	timer_writel(reg, system_timer + TIMER_PTV);

	return 0;
}

static void tegra_timer_set_mode(enum clock_event_mode mode,
				    struct clock_event_device *evt)
{
	u32 reg;

	timer_writel(0, system_timer + TIMER_PTV);

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		reg = 0xC0000000 | ((1000000/HZ)-1);
		timer_writel(reg, system_timer + TIMER_PTV);
		break;
	case CLOCK_EVT_MODE_ONESHOT:
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	case CLOCK_EVT_MODE_RESUME:
		break;
	}
}

static struct clock_event_device tegra_clockevent = {
	.name		= "timer0",
	.rating		= 300,
	.features	= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC,
	.set_next_event	= tegra_timer_set_next_event,
	.set_mode	= tegra_timer_set_mode,
};

u32 notrace tegra_read_usec_raw(void)
{
	return timer_readl(TIMERUS_CNTR_1US);
}

static u32 notrace tegra_read_sched_clock(void)
{
	return timer_readl(TIMERUS_CNTR_1US);
}

/*
 * tegra_rtc_read - Reads the Tegra RTC registers
 * Care must be taken that this funciton is not called while the
 * tegra_rtc driver could be executing to avoid race conditions
 * on the RTC shadow register
 */
u64 tegra_rtc_read_ms(void)
{
	u32 ms = readl(rtc_base + RTC_MILLISECONDS);
	u32 s = readl(rtc_base + RTC_SHADOW_SECONDS);
	return (u64)s * MSEC_PER_SEC + ms;
}

/*
 * tegra_read_persistent_clock -  Return time from a persistent clock.
 *
 * Reads the time from a source which isn't disabled during PM, the
 * 32k sync timer.  Convert the cycles elapsed since last read into
 * nsecs and adds to a monotonically increasing timespec.
 * Care must be taken that this funciton is not called while the
 * tegra_rtc driver could be executing to avoid race conditions
 * on the RTC shadow register
 */
void tegra_read_persistent_clock(struct timespec *ts)
{
	u64 delta;
	struct timespec *tsp = &persistent_ts;

	last_persistent_ms = persistent_ms;
	persistent_ms = tegra_rtc_read_ms();
	delta = persistent_ms - last_persistent_ms;

	timespec_add_ns(tsp, delta * NSEC_PER_MSEC);
	*ts = *tsp;
}

static irqreturn_t tegra_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = (struct clock_event_device *)dev_id;
	timer_writel(1<<30, system_timer + TIMER_PCR);
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static struct irqaction tegra_timer_irq = {
	.name		= "timer0",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_TRIGGER_HIGH,
	.handler	= tegra_timer_interrupt,
	.dev_id		= &tegra_clockevent,
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	.irq		= INT_TMR3,
#else
	.irq		= INT_TMR1,
#endif
};

static int tegra_timer_suspend(void)
{
	usec_config = timer_readl(TIMERUS_USEC_CFG);
	return 0;
}

static void tegra_timer_resume(void)
{
	timer_writel(usec_config, TIMERUS_USEC_CFG);
}

static struct syscore_ops tegra_timer_syscore_ops = {
	.suspend = tegra_timer_suspend,
	.resume = tegra_timer_resume,
	.save = tegra_timer_suspend,
	.restore = tegra_timer_resume,
};

#if !defined(CONFIG_HAVE_ARM_TWD) && !defined(CONFIG_ARM_ARCH_TIMER)

static DEFINE_PER_CPU(struct tegra_clock_event_device, percpu_tegra_timer);

static int __cpuinit tegra_local_timer_setup(struct clock_event_device *evt)
{
	unsigned int cpu = smp_processor_id();
	struct tegra_clock_event_device *clkevt;
	int ret;

	clkevt = this_cpu_ptr(&percpu_tegra_timer);
	clkevt->evt = evt;
	sprintf(clkevt->name, "tegra_timer%d", cpu);
	evt->name = clkevt->name;
	evt->cpumask = cpumask_of(cpu);
	evt->features = tegra_cputimer_clockevent.features;
	evt->rating = tegra_cputimer_clockevent.rating;
	evt->set_mode = tegra_cputimer_set_mode;
	evt->set_next_event = tegra_cputimer_set_next_event;
	clockevents_calc_mult_shift(evt, 1000000, 5);
	evt->max_delta_ns =
		clockevent_delta2ns(0x1fffffff, evt);
	evt->min_delta_ns =
		clockevent_delta2ns(0x1, evt);
	tegra_cputimer_irq[cpu].dev_id = clkevt;

	clockevents_register_device(evt);

	ret = setup_irq(tegra_cputimer_irq[cpu].irq, &tegra_cputimer_irq[cpu]);
	if (ret) {
		pr_err("Failed to register CPU timer IRQ for CPU %d: " \
			"irq=%d, ret=%d\n", cpu,
			tegra_cputimer_irq[cpu].irq, ret);
		return ret;
	}

	evt->irq = tegra_cputimer_irq[cpu].irq;
	ret = irq_set_affinity(tegra_cputimer_irq[cpu].irq, cpumask_of(cpu));
	if (ret) {
		pr_err("Failed to set affinity for CPU timer IRQ to " \
			"CPU %d: irq=%d, ret=%d\n", cpu,
			tegra_cputimer_irq[cpu].irq, ret);
		return ret;
	}

	enable_percpu_irq(evt->irq, IRQ_TYPE_LEVEL_HIGH);

	return 0;
}

static void tegra_local_timer_stop(struct clock_event_device *evt)
{
	unsigned int cpu = smp_processor_id();

	evt->set_mode(CLOCK_EVT_MODE_UNUSED, evt);
	remove_irq(evt->irq, &tegra_cputimer_irq[cpu]);
	disable_percpu_irq(evt->irq);
}
static struct local_timer_ops tegra_local_timer_ops __cpuinitdata = {
	.setup  = tegra_local_timer_setup,
	.stop   = tegra_local_timer_stop,
};

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_HOTPLUG_CPU)
void tegra_cputimer_reset_irq_affinity(int cpu)
{
	/* Reassign the affinity of the wake IRQ to CPU0 */
	(void)irq_set_affinity(tegra_cputimer_irq[cpu].irq,
			       cpumask_of(0));
}
#endif

void __init tegra_cpu_timer_init(void)
{
	/* Use SOC timers as cpu timers */
}
void __init tegra30_init_timer(void)
{
}
void tegra3_lp2_set_trigger(unsigned long cycles)
{
	unsigned int cpu = smp_processor_id();
	int base;

	base = cpu_local_timers[cpu];
	if (cycles) {
		timer_writel(0, base + TIMER_PTV);
		if (cycles) {
			u32 reg = 0x80000000 | ((cycles > 1) ? (cycles-1) : 0);
			timer_writel(reg, base + TIMER_PTV);
		}
	}
}

unsigned long tegra3_lp2_timer_remain(void)
{
	unsigned int cpu = smp_processor_id();

	return timer_readl(cpu_local_timers[cpu] + TIMER_PCR) & 0x1ffffffful;

}

int tegra3_is_cpu_wake_timer_ready(unsigned int cpu)
{
	return 1;
}

void tegra3_lp2_timer_cancel_secondary(void)
{
	/* Nothing needs to be done here */
}

void __init tegra_init_late_timer(void)
{
	local_timer_register(&tegra_local_timer_ops);
}
#endif /* !CONFIG_HAVE_ARM_TWD && !CONFIG_ARM_ARCH_TIMER */

extern void __tegra_delay(unsigned long cycles);
extern void __tegra_const_udelay(unsigned long loops);
extern void __tegra_udelay(unsigned long usecs);

void __init tegra_init_timer(void)
{
	struct clk *clk;
	int ret;
	unsigned long rate;

	clk = clk_get_sys("timer", NULL);
	if (IS_ERR(clk)) {
		pr_warn("Unable to get timer clock. Assuming 12Mhz input clock.\n");
		rate = 12000000;
	} else {
		tegra_clk_prepare_enable(clk);
		rate = clk_get_rate(clk);
	}

	/*
	 * rtc registers are used by read_persistent_clock, keep the rtc clock
	 * enabled
	 */
	clk = clk_get_sys("rtc-tegra", NULL);
	if (IS_ERR(clk))
		pr_warn("Unable to get rtc-tegra clock\n");
	else
		tegra_clk_prepare_enable(clk);

	switch (rate) {
	case 12000000:
		timer_writel(0x000b, TIMERUS_USEC_CFG);
		break;
	case 13000000:
		timer_writel(0x000c, TIMERUS_USEC_CFG);
		break;
	case 19200000:
		timer_writel(0x045f, TIMERUS_USEC_CFG);
		break;
	case 26000000:
		timer_writel(0x0019, TIMERUS_USEC_CFG);
		break;
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	case 16800000:
		timer_writel(0x0453, TIMERUS_USEC_CFG);
		break;
	case 38400000:
		timer_writel(0x04BF, TIMERUS_USEC_CFG);
		break;
	case 48000000:
		timer_writel(0x002F, TIMERUS_USEC_CFG);
		break;
#endif
	default:
		if (tegra_platform_is_qt()) {
			timer_writel(0x000c, TIMERUS_USEC_CFG);
			break;
		}
		WARN(1, "Unknown clock rate");
	}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	tegra20_init_timer();
#else
	tegra30_init_timer();
#endif

	ret = clocksource_mmio_init(timer_reg_base + TIMERUS_CNTR_1US,
		"timer_us", 1000000, 300, 32,
		clocksource_mmio_readl_up);
	if (ret) {
		pr_err("%s: Failed to register clocksource: %d\n",
			__func__, ret);
		BUG();
	}

	ret = setup_irq(tegra_timer_irq.irq, &tegra_timer_irq);
	if (ret) {
		pr_err("%s: Failed to register timer IRQ: %d\n",
			__func__, ret);
		BUG();
	}

	clockevents_calc_mult_shift(&tegra_clockevent, 1000000, 5);
	tegra_clockevent.max_delta_ns =
		clockevent_delta2ns(0x1fffffff, &tegra_clockevent);
	tegra_clockevent.min_delta_ns =
		clockevent_delta2ns(0x1, &tegra_clockevent);
	tegra_clockevent.cpumask = cpu_all_mask;
	tegra_clockevent.irq = tegra_timer_irq.irq;
	clockevents_register_device(&tegra_clockevent);

#ifdef CONFIG_ARM_ARCH_TIMER
	/* Architectural timers take precedence over broadcast timers.
	   Only register a broadcast clockevent device if architectural
	   timers do not exist or cannot be initialized. */
	if (tegra_init_arch_timer())
#endif
		/* Architectural timers do not exist or cannot be initialzied.
		   Fall back to using the broadcast timer as the sched clock. */
		setup_sched_clock(tegra_read_sched_clock, 32, 1000000);

	register_syscore_ops(&tegra_timer_syscore_ops);

	late_time_init = tegra_init_late_timer;

#ifndef CONFIG_ARM64 /* FIXME */
	register_persistent_clock(NULL, tegra_read_persistent_clock);
#endif

	//arm_delay_ops.delay		= __tegra_delay;
	//arm_delay_ops.const_udelay	= __tegra_const_udelay;
	//arm_delay_ops.udelay		= __tegra_udelay;
}
