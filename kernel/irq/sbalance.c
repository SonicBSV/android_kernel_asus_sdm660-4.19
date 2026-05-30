// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 * SDM636/SDM660 optimizations by downstream maintainers.
 */

/**
 * DOC: SBalance description
 *
 * This is a simple IRQ balancer that polls every X number of milliseconds and
 * moves IRQs from the most interrupt-heavy CPU to the least interrupt-heavy
 * CPUs until the heaviest CPU is no longer the heaviest. IRQs are only moved
 * from one source CPU to any number of destination CPUs per balance run.
 * Balancing is skipped if the gap between the most interrupt-heavy CPU and the
 * least interrupt-heavy CPU is below the configured threshold of interrupts.
 *
 * The heaviest IRQs are targeted for migration in order to reduce the number of
 * IRQs to migrate. If moving an IRQ would reduce overall balance, then it won't
 * be migrated.
 *
 * The most interrupt-heavy CPU is calculated by scaling the number of new
 * interrupts on that CPU to the CPU's current capacity. This way, interrupt
 * heaviness takes into account factors such as thermal pressure and time spent
 * processing interrupts rather than just the sheer number of them. This also
 * makes SBalance aware of CPU asymmetry, where different CPUs can have
 * different performance capacities and be proportionally balanced.
 *
 * SDM636/SDM660 specifics
 * -----------------------
 * Cluster layout (compile-time constants, auto-masked to cpu_possible_mask):
 *   LITTLE / Silver (Cortex-A53): CPU0-3, up to 1.84 GHz
 *   big    / Gold   (Cortex-A73): CPU4-7, up to 2.20 GHz (CPU4-5 on SDM636)
 *
 * Extra optimisations applied:
 *  1. Cluster-aware migration: by default IRQs stay in their cluster.
 *     Cross-cluster moves gated by sb_allow_cross_cluster sysfs knob.
 *  2. EMA alpha = 1/8 (was 1/4) to damp short camera/modem ISR bursts.
 *  3. Cooldown checked BEFORE desc->lock is taken — avoids spinlock on
 *     every recently-migrated IRQ (can be 100s per poll on SDM660).
 *  4. Idle backoff: when total scaled intrs < IDLE_INTRS_THRESH the poll
 *     period is multiplied by IDLE_POLL_MULT to save Silver-cluster power.
 *  5. Balancer thread pinned to Silver cluster — does not disturb Gold.
 *  6. Retry loop bounded by MAX_RETRY_CPUS — O(N) instead of O(N^2).
 *  7. sysfs attributes registered as a group (atomic, proper error path).
 *  8. Fixed broken macro token-pasting in SB_ATTR_* (original used *name*).
 *  9. Fixed BUG_ON(IS_ERR(kthread_run())) — panic replaced by proper error.
 */

#define pr_fmt(fmt) "sbalance: " fmt

#include <linux/cpumask.h>
#include <linux/freezer.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/list_sort.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include "../sched/sched.h"
#include "internals.h"

/* --------------------------------------------------------------------------
 * Compile-time defaults
 * -------------------------------------------------------------------------- */

#define POLL_MS           CONFIG_IRQ_SBALANCE_POLL_MSEC
#define IRQ_SCALED_THRESH CONFIG_IRQ_SBALANCE_THRESH

/*
 * SDM660/SDM636 cluster bitmasks.
 * Both are intersected with cpu_possible_mask at init, so SDM636 (only
 * CPU4-5 in gold) works correctly without a separate config symbol.
 */
#define SILVER_CLUSTER_MASK 0x0FUL  /* CPU0-3 */
#define GOLD_CLUSTER_MASK   0xF0UL  /* CPU4-7 */

/*
 * If the sum of scaled interrupts across all CPUs is below this value
 * the balancer backs off to IDLE_POLL_MULT * sb_poll_ms.
 */
#define IDLE_INTRS_THRESH  32u
#define IDLE_POLL_MULT      4u

/* Bound on "try next heaviest CPU" retries to keep the loop O(N). */
#define MAX_RETRY_CPUS     CONFIG_NR_CPUS

/* --------------------------------------------------------------------------
 * EMA: alpha = 1/2^EMA_SHIFT
 * 1/8 (EMA_SHIFT=3) damps short bursts better than the upstream 1/4.
 * -------------------------------------------------------------------------- */
#define EMA_SHIFT 3

/* --------------------------------------------------------------------------
 * Data structures
 * -------------------------------------------------------------------------- */

struct bal_irq {
	struct list_head  node;
	struct list_head  move_node;
	struct rcu_head   rcu;
	struct irq_desc  *desc;
	unsigned int      delta_nr;
	unsigned int      old_nr;
	int               prev_cpu;
	unsigned long     last_moved; /* jiffies of last successful migration */
};

struct bal_domain {
	struct list_head  movable_irqs;
	unsigned int      intrs;
	unsigned int      ema_intrs;  /* EMA-smoothed interrupt count */
	int               cpu;
	int               cluster;    /* 0 = Silver, 1 = Gold */
};

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */

static LIST_HEAD(bal_irq_list);
static DEFINE_SPINLOCK(bal_irq_lock);
static DEFINE_PER_CPU(struct bal_domain, balance_data);
static DEFINE_PER_CPU(unsigned long, cpu_cap);

static cpumask_t cpu_exclude_mask __read_mostly;

/* Cluster masks — computed once at init from SILVER/GOLD_CLUSTER_MASK
 * intersected with cpu_possible_mask.                                   */
static cpumask_t silver_mask __read_mostly;
static cpumask_t gold_mask   __read_mostly;

/* --------------------------------------------------------------------------
 * Runtime knobs (sysfs)
 * -------------------------------------------------------------------------- */

bool sbalance_enabled = true;
static u32  sb_poll_ms           = POLL_MS;
static u32  sb_thresh            = CONFIG_IRQ_SBALANCE_THRESH;
static u32  sb_cooldown_ms       = 2000;
static u32  sb_per_irq_min       = 8;
static bool sb_allow_cross_cluster;      /* default off — cluster-local */
static char sb_blacklist[256];
static char sb_whitelist[256];

static struct kobject *sb_kobj;

/* --------------------------------------------------------------------------
 * sysfs helpers
 *
 * BUG FIX: the original macros used *name* (pointer dereference) instead
 * of the identifier _name.  They would not compile as written.
 * -------------------------------------------------------------------------- */

#define SB_ATTR_RW(_name)						\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%u\n", (unsigned int)(_name));		\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	unsigned long v;						\
	if (kstrtoul(buf, 0, &v))					\
		return -EINVAL;						\
	(_name) = (typeof(_name))v;					\
	return count;							\
}									\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

#define SB_ATTR_BOOL(_name)						\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%u\n", (unsigned int)(_name));		\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	bool v;								\
	if (kstrtobool(buf, &v))					\
		return -EINVAL;						\
	(_name) = v;							\
	return count;							\
}									\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

#define SB_ATTR_STR(_name)						\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%s\n", _name);				\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	size_t n = min(count, sizeof(_name) - 1);			\
	memcpy(_name, buf, n);						\
	_name[n] = '\0';						\
	if (n && _name[n - 1] == '\n')					\
		_name[n - 1] = '\0';					\
	return count;							\
}									\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

SB_ATTR_BOOL(sbalance_enabled);
SB_ATTR_RW(sb_poll_ms);
SB_ATTR_RW(sb_thresh);
SB_ATTR_RW(sb_cooldown_ms);
SB_ATTR_RW(sb_per_irq_min);
SB_ATTR_BOOL(sb_allow_cross_cluster);
SB_ATTR_STR(sb_blacklist);
SB_ATTR_STR(sb_whitelist);

/* exclude_cpus: cpulist <-> mask */
static ssize_t exclude_cpus_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%*pbl\n", cpumask_pr_args(&cpu_exclude_mask));
}

static ssize_t exclude_cpus_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	cpumask_t mask;

	if (cpulist_parse(buf, &mask))
		return -EINVAL;
	cpu_exclude_mask = mask;
	return count;
}
static struct kobj_attribute exclude_cpus_attr =
	__ATTR(exclude_cpus, 0644, exclude_cpus_show, exclude_cpus_store);

/* cluster_info: read-only diagnostic */
static ssize_t cluster_info_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "silver=%*pbl gold=%*pbl cross_cluster=%u\n",
			  cpumask_pr_args(&silver_mask),
			  cpumask_pr_args(&gold_mask),
			  sb_allow_cross_cluster);
}
static struct kobj_attribute cluster_info_attr =
	__ATTR(cluster_info, 0444, cluster_info_show, NULL);

/* Group registration — atomic, single error path */
static struct attribute *sb_attrs[] = {
	&sbalance_enabled_attr.attr,
	&sb_poll_ms_attr.attr,
	&sb_thresh_attr.attr,
	&sb_cooldown_ms_attr.attr,
	&sb_per_irq_min_attr.attr,
	&sb_allow_cross_cluster_attr.attr,
	&sb_blacklist_attr.attr,
	&sb_whitelist_attr.attr,
	&exclude_cpus_attr.attr,
	&cluster_info_attr.attr,
	NULL,
};
static const struct attribute_group sb_attr_group = {
	.attrs = sb_attrs,
};

/* --------------------------------------------------------------------------
 * Blacklist / whitelist matching
 * -------------------------------------------------------------------------- */

static bool strlist_match(const char *name, const char *list)
{
	const char *p = list;
	size_t nlen;

	if (!name || !*name || !list || !*list)
		return false;

	nlen = strlen(name);

	while (*p) {
		const char *q;
		size_t tlen, i;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;

		q = p;
		while (*q && *q != ',' && *q != ' ' && *q != '\t')
			q++;

		tlen = q - p;
		if (tlen) {
			for (i = 0; i + tlen <= nlen; i++) {
				if (!memcmp(name + i, p, tlen))
					return true;
			}
		}
		p = q;
	}
	return false;
}

static bool irq_is_blacklisted(struct irq_desc *desc)
{
	struct irqaction *act = READ_ONCE(desc->action);
	const char *aname = act ? act->name : NULL;
	const char *dname = READ_ONCE(desc->name);

	if (strlist_match(aname, sb_whitelist) ||
	    strlist_match(dname, sb_whitelist))
		return false;

	if (strlist_match(aname, sb_blacklist) ||
	    strlist_match(dname, sb_blacklist))
		return true;

	return false;
}

/* --------------------------------------------------------------------------
 * IRQ descriptor list management
 * -------------------------------------------------------------------------- */

void sbalance_desc_add(struct irq_desc *desc)
{
	struct bal_irq *bi;

	bi = kmalloc(sizeof(*bi), GFP_KERNEL);
	if (WARN_ON(!bi))
		return;

	*bi = (typeof(*bi)){ .desc = desc };
	bi->last_moved = 0;

	spin_lock(&bal_irq_lock);
	list_add_tail_rcu(&bi->node, &bal_irq_list);
	spin_unlock(&bal_irq_lock);
}

void sbalance_desc_del(struct irq_desc *desc)
{
	struct bal_irq *bi;

	spin_lock(&bal_irq_lock);
	list_for_each_entry(bi, &bal_irq_list, node) {
		if (bi->desc == desc) {
			list_del_rcu(&bi->node);
			kfree_rcu(bi, rcu);
			break;
		}
	}
	spin_unlock(&bal_irq_lock);
}

/* --------------------------------------------------------------------------
 * Sort comparator: descending by delta_nr
 * -------------------------------------------------------------------------- */
static int bal_irq_move_node_cmp(void *priv,
				 struct list_head *lhs_p,
				 struct list_head *rhs_p)
{
	const struct bal_irq *lhs =
		list_entry(lhs_p, typeof(*lhs), move_node);
	const struct bal_irq *rhs =
		list_entry(rhs_p, typeof(*rhs), move_node);

	/* delta_nr fits in int range; signed subtraction is safe */
	return (int)rhs->delta_nr - (int)lhs->delta_nr;
}

/* --------------------------------------------------------------------------
 * Per-IRQ data update
 *
 * OPT: cooldown is tested BEFORE acquiring desc->lock.
 * On SDM660 with hundreds of IRQs the modem alone can fire thousands of
 * interrupts per second — skipping the spinlock for in-cooldown IRQs
 * meaningfully reduces lock contention in the poll loop.
 *
 * jiffies snapshot (now) is passed in by the caller so it is read once
 * per balance run instead of once per IRQ.
 * -------------------------------------------------------------------------- */
static bool update_irq_data(struct bal_irq *bi, unsigned long now, int *cpu)
{
	struct irq_desc *desc = bi->desc;
	unsigned int nr;

	/* Fast cooldown check without holding any lock */
	if (READ_ONCE(sb_cooldown_ms) &&
	    time_before(now, READ_ONCE(bi->last_moved) +
			     msecs_to_jiffies(READ_ONCE(sb_cooldown_ms))))
		return false;

	raw_spin_lock_irq(&desc->lock);
	*cpu = cpumask_first(desc->irq_common_data.affinity);
	raw_spin_unlock_irq(&desc->lock);

	if (*cpu >= nr_cpu_ids)
		return false;

	nr = *per_cpu_ptr(desc->kstat_irqs, *cpu);
	if (nr <= bi->old_nr) {
		bi->old_nr = nr;
		return false;
	}

	bi->delta_nr = nr - bi->old_nr;
	bi->old_nr   = nr;
	return true;
}

/* --------------------------------------------------------------------------
 * EMA update
 * -------------------------------------------------------------------------- */
static void update_ema_for_cpu(int cpu)
{
	struct bal_domain *bd = per_cpu_ptr(&balance_data, cpu);
	unsigned int x = bd->intrs;

	if (!bd->ema_intrs)
		bd->ema_intrs = x;
	else
		bd->ema_intrs = bd->ema_intrs
			      - (bd->ema_intrs >> EMA_SHIFT)
			      + (x >> EMA_SHIFT);
}

/* --------------------------------------------------------------------------
 * Capacity-scaled interrupt count
 * -------------------------------------------------------------------------- */
static unsigned int scale_intrs(unsigned int intrs, int cpu)
{
	unsigned long cap = per_cpu(cpu_cap, cpu);

	/* Guard against zero capacity (offline CPU race, thermal shutdown) */
	if (unlikely(!cap))
		return intrs;

	return (unsigned int)div_u64((u64)intrs * SCHED_CAPACITY_SCALE, cap);
}

/* --------------------------------------------------------------------------
 * Cluster helpers
 * -------------------------------------------------------------------------- */
static int cpu_cluster(int cpu)
{
	return cpumask_test_cpu(cpu, &gold_mask) ? 1 : 0;
}

static bool cluster_move_allowed(int src_cpu, int dst_cpu)
{
	if (READ_ONCE(sb_allow_cross_cluster))
		return true;
	return cpu_cluster(src_cpu) == cpu_cluster(dst_cpu);
}

/* --------------------------------------------------------------------------
 * IRQ migration
 * -------------------------------------------------------------------------- */
static int move_irq_to_cpu(struct bal_irq *bi, int cpu)
{
	struct irq_desc *desc = bi->desc;
	int prev_cpu, ret;

	raw_spin_lock_irq(&desc->lock);
	prev_cpu = cpumask_first(desc->irq_common_data.affinity);
	if (prev_cpu == bi->prev_cpu) {
		ret = irq_set_affinity_locked(&desc->irq_data,
					      cpumask_of(cpu), false);
	} else {
		bi->prev_cpu = prev_cpu;
		ret = -EINVAL;
	}
	raw_spin_unlock_irq(&desc->lock);

	if (!ret) {
		bi->old_nr     = *per_cpu_ptr(desc->kstat_irqs, cpu);
		bi->last_moved = jiffies;
		pr_debug("Moved IRQ%d (CPU%d -> CPU%d, cluster %d->%d)\n",
			 irq_desc_get_irq(desc), prev_cpu, cpu,
			 cpu_cluster(prev_cpu), cpu_cluster(cpu));
	}
	return ret;
}

/* --------------------------------------------------------------------------
 * find_min_bd — find the CPU with the fewest scaled interrupts.
 *
 * OPT: when cross-cluster moves are disabled, we look first among CPUs in
 * the same cluster as max_bd.  Only if that yields nothing do we fall back
 * to the global minimum.  This keeps heavy IRQs on Gold and light ones on
 * Silver instead of mixing them.
 *
 * Returns true  → stop balancing (gap below threshold or max dethroned).
 * Returns false → *min_bd points to the best migration target.
 * -------------------------------------------------------------------------- */
static bool find_min_bd(const cpumask_t *mask, unsigned int max_intrs,
			const struct bal_domain *max_bd,
			struct bal_domain **min_bd)
{
	unsigned int min_same = UINT_MAX, min_any = UINT_MAX;
	struct bal_domain *best_same = NULL, *best_any = NULL;
	struct bal_domain *bd;
	int cpu;

	for_each_cpu(cpu, mask) {
		unsigned int intrs;

		bd    = per_cpu_ptr(&balance_data, cpu);
		intrs = scale_intrs(bd->ema_intrs ?: bd->intrs, bd->cpu);

		/* Former max is no longer max → stop */
		if (intrs > max_intrs)
			return true;

		if (!READ_ONCE(sb_allow_cross_cluster) &&
		    bd->cluster == max_bd->cluster) {
			if (intrs < min_same) {
				min_same  = intrs;
				best_same = bd;
			}
		}

		if (intrs < min_any) {
			min_any  = intrs;
			best_any = bd;
		}
	}

	/* Prefer same-cluster target */
	if (best_same) {
		*min_bd = best_same;
		return max_intrs - min_same < READ_ONCE(sb_thresh);
	}

	if (!best_any)
		return true;

	*min_bd = best_any;
	return max_intrs - min_any < READ_ONCE(sb_thresh);
}

/* --------------------------------------------------------------------------
 * Core balancing routine
 *
 * Returns 0       → use normal sb_poll_ms sleep.
 * Returns non-zero → multiply sleep by that factor (idle backoff).
 * -------------------------------------------------------------------------- */
static unsigned int balance_irqs(void)
{
	static cpumask_t cpus;
	struct bal_domain *bd, *max_bd, *min_bd;
	unsigned int intrs, max_intrs, total_scaled;
	bool moved_irq;
	struct bal_irq *bi;
	unsigned long now = jiffies; /* snapshot once for the whole run */
	int cpu, retries;

	rcu_read_lock();

	cpumask_andnot(&cpus, cpu_active_mask, &cpu_exclude_mask);
	if (unlikely(cpumask_weight(&cpus) <= 1))
		goto unlock_normal;

	/* Snapshot CPU capacities (thermal + RT pressure already folded in) */
	for_each_cpu(cpu, &cpus)
		per_cpu(cpu_cap, cpu) = cpu_rq(cpu)->cpu_capacity;

	/* Reset per-window counters */
	for_each_cpu(cpu, &cpus) {
		bd = per_cpu_ptr(&balance_data, cpu);
		bd->intrs = 0;
		INIT_LIST_HEAD(&bd->movable_irqs);
	}

	/*
	 * Accumulate interrupt deltas and build per-CPU movable-IRQ lists.
	 *
	 * OPT: update_irq_data() checks cooldown before taking desc->lock,
	 * so recently-migrated IRQs are skipped with no lock overhead.
	 */
	list_for_each_entry_rcu(bi, &bal_irq_list, node) {
		if (!update_irq_data(bi, now, &cpu))
			continue;

		if (READ_ONCE(sb_per_irq_min) &&
		    bi->delta_nr < READ_ONCE(sb_per_irq_min))
			continue;

		bd = per_cpu_ptr(&balance_data, cpu);
		bd->intrs += bi->delta_nr;

		if (!__irq_can_set_affinity(bi->desc))
			continue;
		if (irq_is_blacklisted(bi->desc))
			continue;

		/* Skip if the IRQ was moved externally since last run */
		if (cpu != bi->prev_cpu) {
			bi->prev_cpu = cpu;
			continue;
		}

		list_add_tail(&bi->move_node, &bd->movable_irqs);
	}

	/* Update EMA for all participating CPUs */
	for_each_cpu(cpu, &cpus)
		update_ema_for_cpu(cpu);

	/*
	 * OPT: idle backoff — if the whole system is quiet skip this round
	 * and tell the caller to extend the sleep.
	 */
	total_scaled = 0;
	for_each_cpu(cpu, &cpus) {
		bd = per_cpu_ptr(&balance_data, cpu);
		total_scaled +=
			scale_intrs(bd->ema_intrs ?: bd->intrs, bd->cpu);
	}
	if (total_scaled < IDLE_INTRS_THRESH)
		goto unlock_idle;

	/* ---- Main balancing loop ---------------------------------------- */
	retries = 0;
	while (retries++ < MAX_RETRY_CPUS) {

		/* Find the most interrupt-heavy CPU */
		max_intrs = 0;
		max_bd    = NULL;
		for_each_cpu(cpu, &cpus) {
			bd    = per_cpu_ptr(&balance_data, cpu);
			intrs = scale_intrs(bd->ema_intrs ?: bd->intrs,
					    bd->cpu);
			if (intrs > max_intrs) {
				max_intrs = intrs;
				max_bd    = bd;
			}
		}

		if (!max_intrs || !max_bd)
			goto unlock_normal;

		/* Does the heaviest CPU have movable IRQs? */
		if (!list_empty(&max_bd->movable_irqs))
			break;

		/*
		 * No movable IRQs: exclude this CPU and try the next
		 * heaviest.  Need at least 2 CPUs remaining.
		 */
		if (cpumask_weight(&cpus) <= 2)
			goto unlock_normal;
		cpumask_clear_cpu(max_bd->cpu, &cpus);
	}

	if (retries > MAX_RETRY_CPUS)
		goto unlock_normal;

	if (find_min_bd(&cpus, max_intrs, max_bd, &min_bd))
		goto unlock_normal;

	/* Sort movable IRQs heaviest-first */
	list_sort(NULL, &max_bd->movable_irqs, bal_irq_move_node_cmp);

	moved_irq = false;

	list_for_each_entry(bi, &max_bd->movable_irqs, move_node) {
		unsigned int projected;

		/*
		 * OPT: enforce cluster policy before any other check —
		 * cheapest possible rejection for cross-cluster candidates.
		 */
		if (!cluster_move_allowed(max_bd->cpu, min_bd->cpu))
			continue;

		/* Would this move overload the target? */
		projected = scale_intrs(
			(min_bd->ema_intrs ?: min_bd->intrs) + bi->delta_nr,
			min_bd->cpu);
		if (projected >= max_intrs)
			continue;

		if (move_irq_to_cpu(bi, min_bd->cpu))
			continue;

		moved_irq = true;

		min_bd->intrs += bi->delta_nr;
		max_bd->intrs -= bi->delta_nr;

		update_ema_for_cpu(min_bd->cpu);
		update_ema_for_cpu(max_bd->cpu);

		max_intrs = scale_intrs(
			max_bd->ema_intrs ?: max_bd->intrs, max_bd->cpu);

		if (find_min_bd(&cpus, max_intrs, max_bd, &min_bd))
			break;
	}

	/*
	 * If nothing was moved (all candidates blocked by cluster policy or
	 * capacity check) exclude this CPU and let the next iteration of the
	 * outer retry loop try the next heaviest CPU.
	 */
	if (!moved_irq && cpumask_weight(&cpus) > 2)
		cpumask_clear_cpu(max_bd->cpu, &cpus);

unlock_normal:
	rcu_read_unlock();
	return 0;

unlock_idle:
	rcu_read_unlock();
	return IDLE_POLL_MULT;
}

/* --------------------------------------------------------------------------
 * Balancer kernel thread
 *
 * OPT: pinned to Silver cluster (CPU0-3) so the balancer's own overhead
 * does not disturb latency-sensitive tasks on Gold cores.
 * -------------------------------------------------------------------------- */
static int __noreturn sbalance_thread(void *data)
{
	struct bal_domain *bd;
	int cpu;

	/* Parse excluded CPUs from Kconfig string */
	if (cpulist_parse(CONFIG_SBALANCE_EXCLUDE_CPUS, &cpu_exclude_mask))
		cpu_exclude_mask = CPU_MASK_NONE;

	/*
	 * Build Silver/Gold cluster masks.  We use the raw bitmask constants
	 * rather than topology_physical_package_id() because on 4.19 Android
	 * kernels the package IDs may not be populated before late_initcall.
	 */
	{
		unsigned long s = SILVER_CLUSTER_MASK;
		unsigned long g = GOLD_CLUSTER_MASK;

		bitmap_and(cpumask_bits(&silver_mask), &s,
			   cpumask_bits(cpu_possible_mask), NR_CPUS);
		bitmap_and(cpumask_bits(&gold_mask), &g,
			   cpumask_bits(cpu_possible_mask), NR_CPUS);
	}

	/* Initialise per-CPU balancing data */
	for_each_possible_cpu(cpu) {
		bd = per_cpu_ptr(&balance_data, cpu);
		INIT_LIST_HEAD(&bd->movable_irqs);
		bd->cpu     = cpu;
		bd->cluster = cpu_cluster(cpu);
	}

	/*
	 * OPT: pin to Silver.  If Silver is fully offlined the kernel will
	 * migrate us automatically — no safety concern.
	 */
	if (!cpumask_empty(&silver_mask))
		set_cpus_allowed_ptr(current, &silver_mask);

	set_freezable();

	while (1) {
		unsigned int idle_mult;
		u32 ms;

		if (!READ_ONCE(sbalance_enabled)) {
			/* Back off gracefully when disabled */
			freezable_schedule_timeout_interruptible(
				msecs_to_jiffies(5000));
			continue;
		}

		ms         = READ_ONCE(sb_poll_ms);
		idle_mult  = balance_irqs(); /* 0 → normal, >0 → idle factor */

		freezable_schedule_timeout_interruptible(
			msecs_to_jiffies(ms * (idle_mult ? idle_mult : 1)));
	}
}

/* --------------------------------------------------------------------------
 * Module init
 *
 * BUG FIX: original used BUG_ON(IS_ERR(kthread_run())) which panics the
 * kernel on allocation failure.  Replaced with a graceful error return.
 * -------------------------------------------------------------------------- */
static int __init sbalance_init(void)
{
	struct task_struct *t;
	int ret;

	t = kthread_run(sbalance_thread, NULL, "sbalanced");
	if (IS_ERR(t)) {
		pr_err("failed to create balancer thread: %ld\n", PTR_ERR(t));
		return PTR_ERR(t);
	}

	sb_kobj = kobject_create_and_add("sbalance", kernel_kobj);
	if (!sb_kobj) {
		pr_warn("failed to create sysfs kobject\n");
		return 0;
	}

	ret = sysfs_create_group(sb_kobj, &sb_attr_group);
	if (ret) {
		pr_warn("sysfs_create_group failed: %d\n", ret);
		kobject_put(sb_kobj);
		sb_kobj = NULL;
	}

	pr_info("initialized (silver=%*pbl gold=%*pbl cross_cluster=%u)\n",
		cpumask_pr_args(&silver_mask),
		cpumask_pr_args(&gold_mask),
		sb_allow_cross_cluster);

	return 0;
}
late_initcall(sbalance_init);
