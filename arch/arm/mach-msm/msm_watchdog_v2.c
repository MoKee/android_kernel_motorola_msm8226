/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/of.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <mach/scm.h>
#include <mach/msm_memory_dump.h>
#include <mach/subsystem_restart.h>
#ifdef CONFIG_MSM_WATCHDOG_CTX_PRINT
#include <linux/mm.h>
#include <linux/persistent_ram.h>
#include <asm/bootinfo.h>
#include <asm/stacktrace.h>
#include <asm/traps.h>
#include <mach/socinfo.h>
#endif

#define MODULE_NAME "msm_watchdog"
#define WDT0_ACCSCSSNBARK_INT 0
#define TCSR_WDT_CFG	0x30
#define WDT0_RST	0x04
#define WDT0_EN		0x08
#define WDT0_STS	0x0C
#define WDT0_BARK_TIME	0x10
#define WDT0_BITE_TIME	0x14

#define MASK_SIZE		32
#define SCM_SET_REGSAVE_CMD	0x2
#define SCM_SVC_SEC_WDOG_DIS	0x7

static struct workqueue_struct *wdog_wq;
static struct msm_watchdog_data *g_wdog_dd;

struct msm_watchdog_data {
	unsigned int __iomem phys_base;
	size_t size;
	void __iomem *base;
	struct device *dev;
	unsigned int pet_time;
	unsigned int bark_time;
	unsigned int bark_irq;
	unsigned int bite_irq;
	bool do_ipi_ping;
	unsigned long long last_pet;
	unsigned min_slack_ticks;
	unsigned long long min_slack_ns;
	unsigned long scm_regsave;		/* phys */
	unsigned long scm_regsave_size;
	cpumask_t alive_mask;
	struct mutex disable_lock;
	struct work_struct init_dogwork_struct;
	struct delayed_work dogwork_struct;
	bool irq_ppi;
	struct msm_watchdog_data __percpu **wdog_cpu_dd;
	struct notifier_block panic_blk;
};

/*
 * On the kernel command line specify
 * msm_watchdog_v2.enable=1 to enable the watchdog
 * By default watchdog is turned on
 */
static int enable = 1;
module_param(enable, int, 0);

/*
 * On the kernel command line specify
 * msm_watchdog_v2.WDT_HZ=<clock val in HZ> to set Watchdog
 * ticks. By default it is set to 32765.
 */
static long WDT_HZ = 32765;
module_param(WDT_HZ, long, 0);

static void pet_watchdog_work(struct work_struct *work);
static void init_watchdog_work(struct work_struct *work);

static void dump_cpu_alive_mask(struct msm_watchdog_data *wdog_dd)
{
	static char alive_mask_buf[MASK_SIZE];
	cpulist_scnprintf(alive_mask_buf, MASK_SIZE,
						&wdog_dd->alive_mask);
	printk(KERN_INFO "cpu alive mask from last pet %s\n", alive_mask_buf);
}

static int msm_watchdog_suspend(struct device *dev)
{
	struct msm_watchdog_data *wdog_dd =
			(struct msm_watchdog_data *)dev_get_drvdata(dev);
	if (!enable)
		return 0;
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	__raw_writel(0, wdog_dd->base + WDT0_EN);
	mb();
	return 0;
}

static int msm_watchdog_resume(struct device *dev)
{
	struct msm_watchdog_data *wdog_dd =
			(struct msm_watchdog_data *)dev_get_drvdata(dev);
	if (!enable)
		return 0;
	__raw_writel(1, wdog_dd->base + WDT0_EN);
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	mb();
	return 0;
}

void msm_watchdog_reset(unsigned int timeout)
{
	unsigned long flags;
	void __iomem *msm_wdt_base;

	local_irq_save(flags);

	if (timeout > 60)
		timeout = 60;

	msm_wdt_base = g_wdog_dd->base;
	__raw_writel(WDT_HZ * timeout, msm_wdt_base + WDT0_BARK_TIME);
	__raw_writel(WDT_HZ * (timeout + 2), msm_wdt_base + WDT0_BITE_TIME);
	__raw_writel(1, msm_wdt_base + WDT0_EN);
	__raw_writel(1, msm_wdt_base + WDT0_RST);

	for (timeout += 2; timeout > 0; timeout--)
		mdelay(1000);

	for (timeout = 2; timeout > 0; timeout--) {
		__raw_writel(0, msm_wdt_base + WDT0_BARK_TIME);
		__raw_writel(WDT_HZ, msm_wdt_base + WDT0_BITE_TIME);
		__raw_writel(1, msm_wdt_base + WDT0_RST);
		mdelay(1000);
	}

	for (timeout = 2; timeout > 0; timeout--) {
		__raw_writel(WDT_HZ, msm_wdt_base + WDT0_BARK_TIME);
		__raw_writel(0, msm_wdt_base + WDT0_BITE_TIME);
		__raw_writel(1, msm_wdt_base + WDT0_RST);
		mdelay(1000);
	}
	pr_err("Watchdog reset has failed\n");

	local_irq_restore(flags);
}

static int panic_wdog_handler(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	struct msm_watchdog_data *wdog_dd = container_of(this,
				struct msm_watchdog_data, panic_blk);
	if (panic_timeout == 0) {
		__raw_writel(0, wdog_dd->base + WDT0_EN);
		mb();
	} else {
		__raw_writel(WDT_HZ * (panic_timeout + 4),
				wdog_dd->base + WDT0_BARK_TIME);
		__raw_writel(WDT_HZ * (panic_timeout + 4),
				wdog_dd->base + WDT0_BITE_TIME);
		__raw_writel(1, wdog_dd->base + WDT0_RST);
	}
	return NOTIFY_DONE;
}

static void wdog_disable(struct msm_watchdog_data *wdog_dd)
{
	__raw_writel(0, wdog_dd->base + WDT0_EN);
	mb();
	if (wdog_dd->irq_ppi) {
		disable_percpu_irq(wdog_dd->bark_irq);
		free_percpu_irq(wdog_dd->bark_irq, wdog_dd->wdog_cpu_dd);
	} else
		devm_free_irq(wdog_dd->dev, wdog_dd->bark_irq, wdog_dd);
	enable = 0;
	/*Ensure all cpus see update to enable*/
	smp_mb();
	atomic_notifier_chain_unregister(&panic_notifier_list,
						&wdog_dd->panic_blk);
	cancel_delayed_work_sync(&wdog_dd->dogwork_struct);
	/* may be suspended after the first write above */
	__raw_writel(0, wdog_dd->base + WDT0_EN);
	mb();
	pr_info("MSM Apps Watchdog deactivated.\n");
}

struct wdog_disable_work_data {
	struct work_struct work;
	struct completion complete;
	struct msm_watchdog_data *wdog_dd;
};

static void wdog_disable_work(struct work_struct *work)
{
	struct wdog_disable_work_data *work_data =
		container_of(work, struct wdog_disable_work_data, work);
	wdog_disable(work_data->wdog_dd);
	complete(&work_data->complete);
}

static ssize_t wdog_disable_get(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret;
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);

	mutex_lock(&wdog_dd->disable_lock);
	ret = snprintf(buf, PAGE_SIZE, "%d\n", enable == 0 ? 1 : 0);
	mutex_unlock(&wdog_dd->disable_lock);
	return ret;
}

static ssize_t wdog_disable_set(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	u8 disable;
	struct wdog_disable_work_data work_data;
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 10, &disable);
	if (ret) {
		dev_err(wdog_dd->dev, "invalid user input\n");
		return ret;
	}
	if (disable == 1) {
		mutex_lock(&wdog_dd->disable_lock);
		if (enable == 0) {
			pr_info("MSM Apps Watchdog already disabled\n");
			mutex_unlock(&wdog_dd->disable_lock);
			return count;
		}
		disable = 1;
		ret = scm_call(SCM_SVC_BOOT, SCM_SVC_SEC_WDOG_DIS, &disable,
						sizeof(disable), NULL, 0);
		if (ret) {
			dev_err(wdog_dd->dev,
					"Failed to deactivate secure wdog\n");
			mutex_unlock(&wdog_dd->disable_lock);
			return -EIO;
		}
		work_data.wdog_dd = wdog_dd;
		init_completion(&work_data.complete);
		INIT_WORK_ONSTACK(&work_data.work, wdog_disable_work);
		queue_work_on(0, wdog_wq, &work_data.work);
		wait_for_completion(&work_data.complete);
		mutex_unlock(&wdog_dd->disable_lock);
	} else {
		pr_err("invalid operation, only disable = 1 supported\n");
		return -EINVAL;
	}
	return count;
}

static DEVICE_ATTR(disable, S_IWUSR | S_IRUSR, wdog_disable_get,
							wdog_disable_set);

static void pet_watchdog(struct msm_watchdog_data *wdog_dd)
{
	int slack, i, count, prev_count = 0;
	unsigned long long time_ns;
	unsigned long long slack_ns;
	unsigned long long bark_time_ns = wdog_dd->bark_time * 1000000ULL;

	for (i = 0; i < 2; i++) {
		count = (__raw_readl(wdog_dd->base + WDT0_STS) >> 1) & 0xFFFFF;
		if (count != prev_count) {
			prev_count = count;
			i = 0;
		}
	}
	slack = ((wdog_dd->bark_time * WDT_HZ) / 1000) - count;
	if (slack < wdog_dd->min_slack_ticks)
		wdog_dd->min_slack_ticks = slack;
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	time_ns = sched_clock();
	slack_ns = (wdog_dd->last_pet + bark_time_ns) - time_ns;
	if (slack_ns < wdog_dd->min_slack_ns)
		wdog_dd->min_slack_ns = slack_ns;
	wdog_dd->last_pet = time_ns;
}

void g_pet_watchdog(void)
{
	pet_watchdog(g_wdog_dd);
}

static void keep_alive_response(void *info)
{
	int cpu = smp_processor_id();
	struct msm_watchdog_data *wdog_dd = (struct msm_watchdog_data *)info;
	cpumask_set_cpu(cpu, &wdog_dd->alive_mask);
	smp_mb();
}

/*
 * If this function does not return, it implies one of the
 * other cpu's is not responsive.
 */
static void ping_other_cpus(struct msm_watchdog_data *wdog_dd)
{
	int cpu;
	cpumask_clear(&wdog_dd->alive_mask);
	smp_mb();
	for_each_cpu(cpu, cpu_online_mask)
		smp_call_function_single(cpu, keep_alive_response, wdog_dd, 1);
}

static void pet_watchdog_work(struct work_struct *work)
{
	unsigned long delay_time;
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct msm_watchdog_data *wdog_dd = container_of(delayed_work,
						struct msm_watchdog_data,
							dogwork_struct);
	delay_time = msecs_to_jiffies(wdog_dd->pet_time);
	if (enable) {
		if (wdog_dd->do_ipi_ping)
			ping_other_cpus(wdog_dd);
		pet_watchdog(wdog_dd);
	}
	/* Check again before scheduling *
	 * Could have been changed on other cpu */
	if (enable)
		queue_delayed_work_on(0, wdog_wq,
				&wdog_dd->dogwork_struct, delay_time);
}

static int msm_watchdog_remove(struct platform_device *pdev)
{
	struct wdog_disable_work_data work_data;
	struct msm_watchdog_data *wdog_dd =
			(struct msm_watchdog_data *)platform_get_drvdata(pdev);

	mutex_lock(&wdog_dd->disable_lock);
	if (enable) {
		work_data.wdog_dd = wdog_dd;
		init_completion(&work_data.complete);
		INIT_WORK_ONSTACK(&work_data.work, wdog_disable_work);
		queue_work_on(0, wdog_wq, &work_data.work);
		wait_for_completion(&work_data.complete);
	}
	mutex_unlock(&wdog_dd->disable_lock);
	device_remove_file(wdog_dd->dev, &dev_attr_disable);
	if (wdog_dd->irq_ppi)
		free_percpu(wdog_dd->wdog_cpu_dd);
	printk(KERN_INFO "MSM Watchdog Exit - Deactivated\n");
	destroy_workqueue(wdog_wq);
	kfree(wdog_dd);
	return 0;
}

static irqreturn_t wdog_bark_handler(int irq, void *dev_id)
{
	struct msm_watchdog_data *wdog_dd = (struct msm_watchdog_data *)dev_id;
	unsigned long nanosec_rem;
	unsigned long long t = sched_clock();

	nanosec_rem = do_div(t, 1000000000);
	printk(KERN_INFO "Watchdog bark! Now = %lu.%06lu\n", (unsigned long) t,
		nanosec_rem / 1000);

	nanosec_rem = do_div(wdog_dd->last_pet, 1000000000);
	printk(KERN_INFO "Watchdog last pet at %lu.%06lu\n", (unsigned long)
		wdog_dd->last_pet, nanosec_rem / 1000);
	if (wdog_dd->do_ipi_ping)
		dump_cpu_alive_mask(wdog_dd);
	printk(KERN_INFO "Causing a watchdog bite!");
	__raw_writel(1, wdog_dd->base + WDT0_BITE_TIME);
	mb();
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	mb();
	/* Delay to make sure bite occurs */
	mdelay(1);
	pr_err("Wdog - STS: 0x%x, CTL: 0x%x, BARK TIME: 0x%x, BITE TIME: 0x%x",
		__raw_readl(wdog_dd->base + WDT0_STS),
		__raw_readl(wdog_dd->base + WDT0_EN),
		__raw_readl(wdog_dd->base + WDT0_BARK_TIME),
		__raw_readl(wdog_dd->base + WDT0_BITE_TIME));
	PR_BUG("Failed to cause a watchdog bite! - Falling back to kernel panic!");
	return IRQ_HANDLED;
}

static irqreturn_t wdog_ppi_bark(int irq, void *dev_id)
{
	struct msm_watchdog_data *wdog_dd =
			*(struct msm_watchdog_data **)(dev_id);
	return wdog_bark_handler(irq, wdog_dd);
}

#ifdef CONFIG_MSM_WATCHDOG_CTX_PRINT

#define TZBSP_SC_STATUS_NS_BIT		0x01
#define TZBSP_SC_STATUS_WDT		0x02
#define TZBSP_SC_STATUS_SGI		0x04
#define TZBSP_SC_STATUS_WARM_BOOT	0x08

#define TZBSP_DUMP_CTX_MAGIC		0x44434151
#define TZBSP_DUMP_CTX_VERSION		2

#define MSM_WDT_CTX_SIG			0x77647473
#define MSM_WDT_CTX_REV			0x00010003

#define NR_CPUS_MAX			4
#define NR_CPUS_2			2
#define NR_CPUS_4			4

#define RET_STAGE_SHIFT			(28)
#define RET_SECTION_SHIFT		(24)
#define RET_ERR_SHIFT			(16)
#define RET_MISC_SHIFT			(12)

#define RET_TZ				(0xfu << RET_STAGE_SHIFT)
#define RET_SBL2			(0xeu << RET_STAGE_SHIFT)

#define ERR_NONE			(0xffu << RET_ERR_SHIFT)

struct tzbsp_mon_cpu_ctx_s {
	u32 mon_lr;
	u32 mon_spsr;
	u32 usr_r0;
	u32 usr_r1;
	u32 usr_r2;
	u32 usr_r3;
	u32 usr_r4;
	u32 usr_r5;
	u32 usr_r6;
	u32 usr_r7;
	u32 usr_r8;
	u32 usr_r9;
	u32 usr_r10;
	u32 usr_r11;
	u32 usr_r12;
	u32 usr_r13;
	u32 usr_r14;
	u32 irq_spsr;
	u32 irq_r13;
	u32 irq_r14;
	u32 svc_spsr;
	u32 svc_r13;
	u32 svc_r14;
	u32 abt_spsr;
	u32 abt_r13;
	u32 abt_r14;
	u32 und_spsr;
	u32 und_r13;
	u32 und_r14;
	u32 fiq_spsr;
	u32 fiq_r8;
	u32 fiq_r9;
	u32 fiq_r10;
	u32 fiq_r11;
	u32 fiq_r12;
	u32 fiq_r13;
	u32 fiq_r14;
};

struct tzbsp_cpu_ctx_s {
	struct tzbsp_mon_cpu_ctx_s saved_ctx;
	u32 mon_sp;
	u32 wdog_pc;
};

/* Structure of the entire non-secure context dump buffer. Because TZ is single
 * entry only a single secure context is saved. */
struct tzbsp_dump_buf_s0 {
	u32 magic;
	u32 version;
	u32 cpu_count;
};

struct tzbsp_dump_buf_s2 {
	u32 magic;
	u32 version;
	u32 cpu_count;
	u32 sc_status[NR_CPUS_2];
	struct tzbsp_cpu_ctx_s sc_ns[NR_CPUS_2];
	struct tzbsp_mon_cpu_ctx_s sec;
	u32 wdt_sts[NR_CPUS_2];
};

struct tzbsp_dump_buf_s4 {
	u32 magic;
	u32 version;
	u32 cpu_count;
	u32 sc_status[NR_CPUS_4];
	struct tzbsp_cpu_ctx_s sc_ns[NR_CPUS_4];
	struct tzbsp_mon_cpu_ctx_s sec;
	u32 wdt_sts[NR_CPUS_4];
};

union tzbsp_dump_buf_s {
	struct tzbsp_dump_buf_s0 s0;
	struct tzbsp_dump_buf_s2 s2;
	struct tzbsp_dump_buf_s4 s4;
};

struct msm_wdt_lnx_info {
	u32 tsk_size;
	u32 ti_tsk_offset;
} __packed __aligned(4);

struct msm_wdt_ctx_info {
	struct msm_wdt_lnx_info lnx;
	u32 size_tz;
	u32 rev_tz;
	u32 size;
	u32 cpu_count;
	u32 ret;
	u32 rev;
	u32 sig;
} __packed __aligned(4);

struct msm_wdt_page1 {
	union tzbsp_dump_buf_s tzbsp;
	u8 spare[PAGE_SIZE - sizeof(union tzbsp_dump_buf_s)
			- sizeof(struct msm_wdt_ctx_info)];
	struct msm_wdt_ctx_info cinfo;
} __packed __aligned(PAGE_SIZE);

struct msm_wdt_cpy {
	u32 from;
	u32 to;
	u32 size;
} __packed __aligned(4);

#define LNX_STACK	0
#define LNX_TASK	1
#define LNX_CTX_AREAS	2

struct msm_wdt_pcp {
	u32 ret;
	u32 stack_va;
	struct msm_wdt_cpy jobs[LNX_CTX_AREAS];
} __packed __aligned(4);

struct msm_wdt_ctx {
	struct msm_wdt_page1 p1;
} __packed __aligned(PAGE_SIZE);

/*
 *	---------	---------------------
 *	|	|	| tzbsp_dump_buf_s
 *	| page1 |	| padding
 *	|	|	| msm_wdt_ctx_info
 *	---------	---------------------
 *	| page2 |	| msm_wdt_pcp[0], ...
 *	| ...	|	| task_struct[0], ...
 *	| pagex |	| padding
 *	---------	---------------------
 *	| ...	|	| stack[0], ...
 *	|	|	|
 */

#define MSM_WDT_CTX_PCP_SIZE	(sizeof(struct msm_wdt_pcp) * get_core_count())
#define MSM_WDT_CTX_TSK_SIZE	(sizeof(struct task_struct) * get_core_count())

#define MSM_WDT_CTX_MID_SIZE	(ALIGN((MSM_WDT_CTX_PCP_SIZE + \
					MSM_WDT_CTX_TSK_SIZE), PAGE_SIZE))
#define MSM_WDT_CTX_THREAD_SIZE	(THREAD_SIZE * get_core_count())
#define MSM_WDT_CTX_SIZE	(sizeof(struct msm_wdt_ctx)	\
				+ MSM_WDT_CTX_MID_SIZE \
				+ MSM_WDT_CTX_THREAD_SIZE)

#define MSMWDTD(fmt, args...) do {			\
	persistent_ram_ext_oldbuf_print(fmt, ##args);	\
} while (0)

static void msm_wdt_show_status(u32 sc_status, const char *label)
{
	if (!sc_status) {
		MSMWDTD("%s: probably didn't finish dump.\n", label);
		return;
	}
	MSMWDTD("%s: was in %ssecure world.\n", label,
		(sc_status & TZBSP_SC_STATUS_NS_BIT) ? "non-" : "");
	if (sc_status & TZBSP_SC_STATUS_WDT)
		MSMWDTD("%s: experienced a watchdog timeout.\n", label);
	if (sc_status & TZBSP_SC_STATUS_SGI)
		MSMWDTD("%s: some other core experienced a watchdog timeout.\n",
			label);
	if (sc_status & TZBSP_SC_STATUS_WARM_BOOT)
		MSMWDTD("%s: WDT bark occured during TZ warm boot.\n", label);
}

static const char stat_nam[] = TASK_STATE_TO_CHAR_STR;
static void msm_wdt_show_task(struct task_struct *p,
			struct thread_info *ti)
{
	unsigned state;

	state = p->state ? __ffs(p->state) + 1 : 0;
	MSMWDTD("%-15.15s %c", p->comm,
		state < sizeof(stat_nam) - 1 ? stat_nam[state] : '?');
	if (state == TASK_RUNNING)
		MSMWDTD(" running  ");
	else
		MSMWDTD(" %08x ", ti->cpu_context.pc);
	MSMWDTD("pid %6d tgid %6d 0x%08lx\n", task_pid_nr(p), task_tgid_nr(p),
			(unsigned long)ti->flags);
}

static void msm_wdt_show_regs(struct tzbsp_mon_cpu_ctx_s *regs,
				const char *label)
{
	MSMWDTD("%s\n", label);
	MSMWDTD("\tr12: %08x  r11: %08x  r10: %08x  r9 : %08x  r8 : %08x\n",
			regs->usr_r12, regs->usr_r11, regs->usr_r10,
			regs->usr_r9, regs->usr_r8);
	MSMWDTD("\tr7 : %08x  r6 : %08x  r5 : %08x  r4 : %08x\n",
			regs->usr_r7, regs->usr_r6, regs->usr_r5,
			regs->usr_r4);
	MSMWDTD("\tr3 : %08x  r2 : %08x  r1 : %08x  r0 : %08x\n",
			regs->usr_r3, regs->usr_r2, regs->usr_r1,
			regs->usr_r0);
	MSMWDTD("MON:\tlr : %08x  spsr: %08x\n",
			regs->mon_lr, regs->mon_spsr);
	MSMWDTD("SVC:\tlr : %08x  sp : %08x  spsr : %08x\n",
			regs->svc_r14, regs->svc_r13, regs->svc_spsr);
	MSMWDTD("IRQ:\tlr : %08x  sp : %08x  spsr : %08x\n",
			regs->irq_r14, regs->irq_r13, regs->irq_spsr);
	MSMWDTD("ABT:\tlr : %08x  sp : %08x  spsr : %08x\n",
			regs->abt_r14, regs->abt_r13, regs->abt_spsr);
	MSMWDTD("UND:\tlr : %08x  sp : %08x  spsr : %08x\n",
			regs->und_r14, regs->und_r13, regs->und_spsr);
	MSMWDTD("FIQ:\tlr : %08x  sp : %08x  spsr : %08x\n",
			regs->fiq_r14, regs->fiq_r13, regs->fiq_spsr);
	MSMWDTD("\tr12: %08x  r11: %08x  r10: %08x  r9 : %08x  r8 : %08x\n",
			regs->fiq_r12, regs->fiq_r11, regs->fiq_r10,
			regs->fiq_r9, regs->fiq_r8);
	MSMWDTD("USR:\tlr : %08x  sp : %08x\n",
			regs->usr_r14, regs->usr_r13);
}

static void msm_wdt_show_raw_mem(unsigned long addr, int nbytes,
			unsigned long old_addr, const char *label)
{
	int	i, j;
	int	nlines;
	u32	*p;

	MSMWDTD("%s: %#lx: ", label, old_addr);
	if (!virt_addr_valid(old_addr)) {
		MSMWDTD("is not valid kernel address.\n");
		return;
	}
	MSMWDTD("\n");

	/*
	 * round address down to a 32 bit boundary
	 * and always dump a multiple of 32 bytes
	 */
	p = (u32 *)(addr & ~(sizeof(u32) - 1));
	nbytes += (addr & (sizeof(u32) - 1));
	nlines = (nbytes + 31) / 32;


	for (i = 0; i < nlines; i++) {
		/*
		 * just display low 16 bits of address to keep
		 * each line of the dump < 80 characters
		 */
		MSMWDTD("%04lx ", (unsigned long)old_addr & 0xffff);
		for (j = 0; j < 8; j++) {
			MSMWDTD(" %08x", *p);
			++p;
			old_addr += sizeof(*p);
		}
		MSMWDTD("\n");
	}
}

static void msm_wdt_unwind_backtrace(struct tzbsp_mon_cpu_ctx_s *regs,
			unsigned long addr, unsigned long stack)
{
	struct stackframe frame;
	int offset;

	if (!virt_addr_valid(addr)) {
		MSMWDTD("%08lx is not valid kernel address.\n", addr);
		return;
	}
	if ((regs->svc_r13 & ~(THREAD_SIZE - 1)) == addr) {
		frame.fp = (regs->usr_r11 & (THREAD_SIZE - 1)) + stack;
		frame.sp = (regs->svc_r13 & (THREAD_SIZE - 1)) + stack;
		frame.lr = regs->svc_r14;
		frame.pc = regs->mon_lr;
	} else {
		struct thread_info *ti = (struct thread_info *)stack;
		frame.fp = ti->cpu_context.fp - addr + stack;
		frame.sp = ti->cpu_context.sp - addr + stack;
		frame.lr = 0;
		frame.pc = ti->cpu_context.pc;
	}
	offset = (frame.sp - stack - 128) & ~(128 - 1);
	msm_wdt_show_raw_mem(stack, 96, addr, "thread_info");
	msm_wdt_show_raw_mem(stack + offset, THREAD_SIZE - offset,
			addr + offset, "stack");
	while (1) {
		int urc;
		unsigned long where = frame.pc;

		urc = unwind_frame(&frame);
		if (urc < 0)
			break;
		MSMWDTD("[<%08lx>] (%pS) from [<%08lx>] (%pS)\n", where,
			(void *)where, frame.pc, (void *)frame.pc);
		if (in_exception_text(where)) {
			struct pt_regs *excep_regs =
					(struct pt_regs *)(frame.sp);
			if ((excep_regs->ARM_sp & ~(THREAD_SIZE - 1)) == addr) {
				excep_regs->ARM_sp -= addr;
				excep_regs->ARM_sp += stack;
			}
		}
	}
}

static void msm_wdt_ctx_print(struct msm_wdt_ctx *ctx)
{
	int i;
	unsigned long stack_tmp = 0, orr = 0;
	u32 *sc_status, *wdt_sts;
	struct tzbsp_cpu_ctx_s *sc_ns;
	char label[64];
	u8 (*stack)[THREAD_SIZE];
	const int cpu_count = get_core_count();
	struct msm_wdt_pcp *pcp;
	struct task_struct *task;

	switch (cpu_count) {
	case NR_CPUS_2:
		sc_status = ctx->p1.tzbsp.s2.sc_status;
		sc_ns = ctx->p1.tzbsp.s2.sc_ns;
		wdt_sts = ctx->p1.tzbsp.s2.wdt_sts;
		break;
	case NR_CPUS_4:
		sc_status = ctx->p1.tzbsp.s4.sc_status;
		sc_ns = ctx->p1.tzbsp.s4.sc_ns;
		wdt_sts = ctx->p1.tzbsp.s4.wdt_sts;
		break;
	default:
		MSMWDTD("msm_wdt_ctx: unsupported cpu_count %d\n", cpu_count);
		return;
	}
	for (i = 0; i < cpu_count; i++) {
		orr |= wdt_sts[i];
		orr |= sc_ns[i].saved_ctx.mon_lr;
	}
	if (!orr) {
		u32 *tzctx;
		if (bi_powerup_reason() != PU_REASON_WDOG_AP_RESET)
			return;
		for (tzctx = sc_status; tzctx < (u32 *)ctx->p1.spare; tzctx++) {
			orr |= *tzctx;
			if (orr)
				break;
		}
		if (!orr) {
			MSMWDTD("\n*** Might be Watchdog Bite ***\n");
			return;
		} else {
			MSMWDTD("*** Likely Bad Dump ***\n");
		}
	}
	if ((ctx->p1.tzbsp.s0.magic != TZBSP_DUMP_CTX_MAGIC)
		|| (ctx->p1.tzbsp.s0.version != TZBSP_DUMP_CTX_VERSION)
		|| (ctx->p1.tzbsp.s0.cpu_count != cpu_count)) {
		MSMWDTD("msm_wdt_ctx: tzbsp dump buffer mismatch.\n");
		MSMWDTD("Expected: magic 0x%08x, version %d, cpu_count %d\n",
			TZBSP_DUMP_CTX_MAGIC, TZBSP_DUMP_CTX_VERSION,
			cpu_count);
		MSMWDTD("Found:    magic 0x%08x, version %d, cpu_count %d\n",
			ctx->p1.tzbsp.s0.magic, ctx->p1.tzbsp.s0.version,
			ctx->p1.tzbsp.s0.cpu_count);
		return;
	}
	MSMWDTD("sc_status: ");
	for (i = 0; i < cpu_count; i++)
		MSMWDTD("0x%x ", sc_status[i]);
	MSMWDTD("\n");
	for (i = 0; i < cpu_count; i++) {
		snprintf(label, sizeof(label) - 1, "CPU%d", i);
		msm_wdt_show_status(sc_status[i], label);
	}
	MSMWDTD("wdt_sts: ");
	for (i = 0; i < cpu_count; i++)
		MSMWDTD("0x%08x ", wdt_sts[i]);
	MSMWDTD("\n");
	for (i = 0; i < cpu_count; i++) {
		snprintf(label, sizeof(label) - 1, "CPU%d nsec", i);
		msm_wdt_show_regs(&sc_ns[i].saved_ctx, label);
	}
	msm_wdt_show_regs(&sc_ns[cpu_count].saved_ctx, "sec");
	if ((ctx->p1.cinfo.sig != MSM_WDT_CTX_SIG)
		|| (ctx->p1.cinfo.rev_tz != MSM_WDT_CTX_REV)
		|| (ctx->p1.cinfo.size_tz != MSM_WDT_CTX_SIZE)) {
		MSMWDTD("msm_wdt_ctx: linux dump buffer mismatch.\n");
		MSMWDTD("Expected: sig 0x%08x, ver 0x%08x size 0x%08x\n",
			MSM_WDT_CTX_SIG, MSM_WDT_CTX_REV, MSM_WDT_CTX_SIZE);
		MSMWDTD("Found:    sig 0x%08x, ver 0x%08x size 0x%08x\n",
			ctx->p1.cinfo.sig, ctx->p1.cinfo.rev_tz,
			ctx->p1.cinfo.size_tz);
		return;
	}
	/* now dump customized stuff */
	MSMWDTD("\n");
	MSMWDTD("ret 0x%08x\n", ctx->p1.cinfo.ret);
	MSMWDTD("\n");
	pcp = (struct msm_wdt_pcp *)(ctx + 1);
	for (i = 0; i < cpu_count; i++) {
		struct msm_wdt_cpy *job;
		MSMWDTD("CPU%d: ", i);
		MSMWDTD("ret 0x%x ", pcp[i].ret);
		MSMWDTD("stack %08x ", pcp[i].stack_va);
		job = &pcp[i].jobs[LNX_TASK];
		MSMWDTD("%08x -> %08x (%x) ", job->from, job->to, job->size);
		job = &pcp[i].jobs[LNX_STACK];
		MSMWDTD("%08x -> %08x (%x)", job->from, job->to, job->size);
		MSMWDTD("\n");
	}
	if ((ctx->p1.cinfo.ret != (RET_TZ | ERR_NONE)))
		return;
	MSMWDTD("\n");
	task = (struct task_struct *)(pcp + cpu_count);
	stack = (u8 (*)[THREAD_SIZE])(ALIGN((unsigned long)(task + cpu_count),
					PAGE_SIZE));
	if (!IS_ALIGNED((unsigned long)stack, THREAD_SIZE)) {
		stack_tmp = __get_free_pages(GFP_KERNEL, THREAD_SIZE_ORDER);
		if (!stack_tmp)
			pr_err("Allocating temporary storage for stack failed.\n");
	}
	for (i = 0; i < cpu_count; i++) {
		unsigned long addr, data;

		if (pcp[i].ret != (RET_TZ | ERR_NONE))
			continue;
		MSMWDTD("CPU%d: ", i);
		addr = pcp[i].stack_va;
		if (stack_tmp) {
			memcpy((void *)stack_tmp, stack[i], THREAD_SIZE);
			data = stack_tmp;
		} else {
			data = (unsigned long)stack[i];
		}
		msm_wdt_show_task(&task[i], (struct thread_info *)data);
		msm_wdt_unwind_backtrace(&sc_ns[i].saved_ctx, addr, data);
		MSMWDTD("\n");
	}
	if (stack_tmp)
		free_pages(stack_tmp, THREAD_SIZE_ORDER);
}

static void msm_watchdog_ctx_lnx(struct msm_wdt_lnx_info *lnx)
{
	lnx->tsk_size = sizeof(struct task_struct);
	lnx->ti_tsk_offset = offsetof(struct thread_info, task);
}

static void msm_wdt_ctx_reset(struct msm_wdt_ctx *ctx)
{
	memset(ctx, 0, MSM_WDT_CTX_SIZE);
	ctx->p1.cinfo.sig = MSM_WDT_CTX_SIG;
	ctx->p1.cinfo.rev = MSM_WDT_CTX_REV;
	ctx->p1.cinfo.cpu_count = get_core_count();
	ctx->p1.cinfo.size = MSM_WDT_CTX_SIZE;
	msm_watchdog_ctx_lnx(&ctx->p1.cinfo.lnx);
}

static void msm_watchdog_alloc_buf(struct msm_watchdog_data *wdog_dd)
{
	struct platform_device *pdev = to_platform_device(wdog_dd->dev);
	struct device_node *node = pdev->dev.of_node;
	int ret, size;
	unsigned long phys;
	struct msm_wdt_ctx *ctx;

	BUILD_BUG_ON((sizeof(union tzbsp_dump_buf_s)
		+ sizeof(struct msm_wdt_ctx_info)) > PAGE_SIZE);
	BUILD_BUG_ON(CONFIG_NR_CPUS > NR_CPUS_MAX);

	ret = of_property_read_u32(node, "qcom,memory-reservation-size", &size);

	if (ret < 0) {
		dev_err(wdog_dd->dev, "reservation not found.\n");
		goto no_reservation;
	}
	if (size < MSM_WDT_CTX_SIZE) {
		dev_err(wdog_dd->dev, "reserved buf too small: 0x%X < 0x%X\n",
			size, MSM_WDT_CTX_SIZE);
		goto no_reservation;
	}

	/* Memory with size specified in device tree has been
	 * reserved together by mdesc->reserve().
	 */
	phys = allocate_contiguous_ebi_nomap(MSM_WDT_CTX_SIZE, PAGE_SIZE);
	if (!phys) {
		dev_err(wdog_dd->dev, "failed to alloc from mempool\n");
		goto no_reservation;
	}
	wdog_dd->scm_regsave = phys;
	wdog_dd->scm_regsave_size = MSM_WDT_CTX_SIZE;

	dev_dbg(wdog_dd->dev, "buf 0x%08lX size 0x%X (reserved 0x%X)\n",
			phys, MSM_WDT_CTX_SIZE, size);
	ctx = ioremap(phys, MSM_WDT_CTX_SIZE);
	if (!ctx) {
		dev_err(wdog_dd->dev, "cannot remap buffer: %08lX\n", phys);
		return;
	}
	msm_wdt_ctx_print(ctx);
	msm_wdt_ctx_reset(ctx);
	iounmap(ctx);
	return;

no_reservation:
	ctx = (struct msm_wdt_ctx *)__get_free_page(GFP_KERNEL);
	if (ctx) {
		wdog_dd->scm_regsave = virt_to_phys(ctx);
		wdog_dd->scm_regsave_size = PAGE_SIZE;
	} else {
		wdog_dd->scm_regsave = 0;
		wdog_dd->scm_regsave_size = 0;
	}
	return;
}

#else /* CONFIG_MSM_WATCHDOG_CTX_PRINT undefined below */

#define MSM_WDT_CTX_SIZE	PAGE_SIZE

static void msm_watchdog_alloc_buf(struct msm_watchdog_data *wdog_dd)
{
	void *ctx_va = (void *)__get_free_page(GFP_KERNEL);
	if (ctx_va) {
		wdog_dd->scm_regsave = virt_to_phys(ctx_va);
		wdog_dd->scm_regsave_size = PAGE_SIZE;
	} else {
		wdog_dd->scm_regsave = 0;
		wdog_dd->scm_regsave_size = 0;
	}
}
#endif /* CONFIG_MSM_WATCHDOG_CTX_PRINT */

static void configure_bark_dump(struct msm_watchdog_data *wdog_dd)
{
	int ret;
	struct msm_client_dump dump_entry;
	struct {
		unsigned addr;
		int len;
	} cmd_buf;

	msm_watchdog_alloc_buf(wdog_dd);
	if (wdog_dd->scm_regsave) {
		cmd_buf.addr = wdog_dd->scm_regsave;
		cmd_buf.len  = wdog_dd->scm_regsave_size;
		ret = scm_call(SCM_SVC_UTIL, SCM_SET_REGSAVE_CMD,
					&cmd_buf, sizeof(cmd_buf), NULL, 0);
		if (ret)
			pr_err("Setting register save address failed.\n"
				       "Registers won't be dumped on a dog "
				       "bite\n");
		dump_entry.id = MSM_CPU_CTXT;
		dump_entry.start_addr = wdog_dd->scm_regsave;
		dump_entry.end_addr = dump_entry.start_addr +
					wdog_dd->scm_regsave_size;
		ret = msm_dump_table_register(&dump_entry);
		if (ret)
			pr_err("Setting cpu dump region failed\n"
				"Registers wont be dumped during cpu hang\n");
	} else {
		pr_err("Allocating register save space failed\n"
			       "Registers won't be dumped on a dog bite\n");
		/*
		 * No need to bail if allocation fails. Simply don't
		 * send the command, and the secure side will reset
		 * without saving registers.
		 */
	}
}


static void init_watchdog_work(struct work_struct *work)
{
	struct msm_watchdog_data *wdog_dd = container_of(work,
						struct msm_watchdog_data,
							init_dogwork_struct);
	unsigned long delay_time;
	int error;
	u64 timeout;
	int ret;

	if (wdog_dd->irq_ppi) {
		wdog_dd->wdog_cpu_dd = alloc_percpu(struct msm_watchdog_data *);
		if (!wdog_dd->wdog_cpu_dd) {
			dev_err(wdog_dd->dev, "fail to allocate cpu data\n");
			return;
		}
		*__this_cpu_ptr(wdog_dd->wdog_cpu_dd) = wdog_dd;
		ret = request_percpu_irq(wdog_dd->bark_irq, wdog_ppi_bark,
					"apps_wdog_bark",
					wdog_dd->wdog_cpu_dd);
		if (ret) {
			dev_err(wdog_dd->dev, "failed to request bark irq\n");
			free_percpu(wdog_dd->wdog_cpu_dd);
			return;
		}
	} else {
		ret = devm_request_irq(wdog_dd->dev, wdog_dd->bark_irq,
				wdog_bark_handler, IRQF_TRIGGER_RISING,
						"apps_wdog_bark", wdog_dd);
		if (ret) {
			dev_err(wdog_dd->dev, "failed to request bark irq\n");
			return;
		}
	}
	delay_time = msecs_to_jiffies(wdog_dd->pet_time);
	wdog_dd->min_slack_ticks = UINT_MAX;
	wdog_dd->min_slack_ns = ULLONG_MAX;
	configure_bark_dump(wdog_dd);
	timeout = (wdog_dd->bark_time * WDT_HZ)/1000;
	__raw_writel(timeout, wdog_dd->base + WDT0_BARK_TIME);
	__raw_writel(timeout + 3*WDT_HZ, wdog_dd->base + WDT0_BITE_TIME);

	wdog_dd->panic_blk.notifier_call = panic_wdog_handler;
	atomic_notifier_chain_register(&panic_notifier_list,
				       &wdog_dd->panic_blk);
	mutex_init(&wdog_dd->disable_lock);
	queue_delayed_work_on(0, wdog_wq, &wdog_dd->dogwork_struct,
			delay_time);
	__raw_writel(1, wdog_dd->base + WDT0_EN);
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	wdog_dd->last_pet = sched_clock();
	error = device_create_file(wdog_dd->dev, &dev_attr_disable);
	if (error)
		dev_err(wdog_dd->dev, "cannot create sysfs attribute\n");
	if (wdog_dd->irq_ppi)
		enable_percpu_irq(wdog_dd->bark_irq, 0);
	dev_info(wdog_dd->dev, "MSM Watchdog Initialized\n");
	return;
}

static struct of_device_id msm_wdog_match_table[] = {
	{ .compatible = "qcom,msm-watchdog" },
	{}
};

static void __devinit dump_pdata(struct msm_watchdog_data *pdata)
{
	dev_dbg(pdata->dev, "wdog bark_time %d", pdata->bark_time);
	dev_dbg(pdata->dev, "wdog pet_time %d", pdata->pet_time);
	dev_dbg(pdata->dev, "wdog perform ipi ping %d", pdata->do_ipi_ping);
	dev_dbg(pdata->dev, "wdog base address is 0x%x\n", (unsigned int)
								pdata->base);
}

static int __devinit msm_wdog_dt_to_pdata(struct platform_device *pdev,
					struct msm_watchdog_data *pdata)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *wdog_resource;
	int ret;

	wdog_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pdata->size = resource_size(wdog_resource);
	pdata->phys_base = wdog_resource->start;
	if (unlikely(!(devm_request_mem_region(&pdev->dev, pdata->phys_base,
					       pdata->size, "msm-watchdog")))) {

		dev_err(&pdev->dev, "%s cannot reserve watchdog region\n",
								__func__);
		return -ENXIO;
	}
	pdata->base  = devm_ioremap(&pdev->dev, pdata->phys_base,
							pdata->size);
	if (!pdata->base) {
		dev_err(&pdev->dev, "%s cannot map wdog register space\n",
				__func__);
		return -ENXIO;
	}

	pdata->bark_irq = platform_get_irq(pdev, 0);
	pdata->bite_irq = platform_get_irq(pdev, 1);
	ret = of_property_read_u32(node, "qcom,bark-time", &pdata->bark_time);
	if (ret) {
		dev_err(&pdev->dev, "reading bark time failed\n");
		return -ENXIO;
	}
	ret = of_property_read_u32(node, "qcom,pet-time", &pdata->pet_time);
	if (ret) {
		dev_err(&pdev->dev, "reading pet time failed\n");
		return -ENXIO;
	}
	pdata->do_ipi_ping = of_property_read_bool(node, "qcom,ipi-ping");
	if (!pdata->bark_time) {
		dev_err(&pdev->dev, "%s watchdog bark time not setup\n",
								__func__);
		return -ENXIO;
	}
	if (!pdata->pet_time) {
		dev_err(&pdev->dev, "%s watchdog pet time not setup\n",
								__func__);
		return -ENXIO;
	}
	pdata->irq_ppi = irq_is_per_cpu(pdata->bark_irq);
	dump_pdata(pdata);
	return 0;
}

static int __devinit msm_watchdog_probe(struct platform_device *pdev)
{
	int ret;
	struct msm_watchdog_data *wdog_dd;

	wdog_wq = alloc_workqueue("wdog", WQ_HIGHPRI, 0);
	if (!wdog_wq) {
		pr_err("Failed to allocate watchdog workqueue\n");
		return -EIO;
	}

	if (!pdev->dev.of_node || !enable)
		return -ENODEV;
	wdog_dd = kzalloc(sizeof(struct msm_watchdog_data), GFP_KERNEL);
	if (!wdog_dd)
		return -EIO;
	ret = msm_wdog_dt_to_pdata(pdev, wdog_dd);
	if (ret)
		goto err;
	wdog_dd->dev = &pdev->dev;
	platform_set_drvdata(pdev, wdog_dd);
	cpumask_clear(&wdog_dd->alive_mask);
	INIT_WORK(&wdog_dd->init_dogwork_struct, init_watchdog_work);
	INIT_DELAYED_WORK(&wdog_dd->dogwork_struct, pet_watchdog_work);
	g_wdog_dd = wdog_dd;
	queue_work_on(0, wdog_wq, &wdog_dd->init_dogwork_struct);
	return 0;
err:
	destroy_workqueue(wdog_wq);
	kzfree(wdog_dd);
	return ret;
}

static const struct dev_pm_ops msm_watchdog_dev_pm_ops = {
	.suspend_noirq = msm_watchdog_suspend,
	.resume_noirq = msm_watchdog_resume,
};

static struct platform_driver msm_watchdog_driver = {
	.probe = msm_watchdog_probe,
	.remove = msm_watchdog_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.pm = &msm_watchdog_dev_pm_ops,
		.of_match_table = msm_wdog_match_table,
	},
};

static int __devinit init_watchdog(void)
{
	return platform_driver_register(&msm_watchdog_driver);
}

pure_initcall(init_watchdog);
MODULE_DESCRIPTION("MSM Watchdog Driver");
MODULE_LICENSE("GPL v2");
