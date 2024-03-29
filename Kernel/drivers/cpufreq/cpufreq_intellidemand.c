/*
 *  drivers/cpufreq/cpufreq_intellidemand.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/earlysuspend.h>

#define _LIMIT_LCD_OFF_CPU_MAX_FREQ_

/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_FREQUENCY_DOWN_DIFFERENTIAL		(10)
#define DEF_FREQUENCY_UP_THRESHOLD		(98)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(100000)
#define MICRO_FREQUENCY_DOWN_DIFFERENTIAL	(3)
#define MICRO_FREQUENCY_UP_THRESHOLD		(95)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)
#define MIN_FREQUENCY_UP_THRESHOLD		(11)
#define MAX_FREQUENCY_UP_THRESHOLD		(100)
#define MIN_FREQUENCY_DOWN_DIFFERENTIAL		(1)

/*
 * The polling frequency of this governor depends on the capability of
 * the processor. Default polling frequency is 1000 times the transition
 * latency of the processor. The governor will work on any processor with
 * transition latency <= 10mS, using appropriate sampling
 * rate.
 * For CPUs with transition latency > 10mS (mostly drivers with CPUFREQ_ETERNAL)
 * this governor will not work.
 * All times here are in uS.
 */
#define MIN_SAMPLING_RATE_RATIO			(2)

static unsigned int min_sampling_rate;

#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

static void do_dbs_timer(struct work_struct *work);
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				unsigned int event);

#ifdef _LIMIT_LCD_OFF_CPU_MAX_FREQ_
#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend cpufreq_gov_early_suspend;
static unsigned int cpufreq_gov_lcd_status;
#endif
#endif

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_INTELLIDEMAND
static
#endif
struct cpufreq_governor cpufreq_gov_intellidemand = {
       .name                   = "intellidemand",
       .governor               = cpufreq_governor_dbs,
       .max_transition_latency = TRANSITION_LATENCY_LIMIT,
       .owner                  = THIS_MODULE,
};

/* Sampling types */
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_iowait;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	struct cpufreq_frequency_table *freq_table;
	unsigned int freq_lo;
	unsigned int freq_lo_jiffies;
	unsigned int freq_hi_jiffies;
	unsigned int rate_mult;
	int cpu;
	unsigned int sample_type:1;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, od_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * dbs_mutex protects data in dbs_tuners_ins from concurrent changes on
 * different CPUs. It protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct workqueue_struct	*kintellidemand_wq;

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int down_differential;
	unsigned int ignore_nice;
	unsigned int sampling_down_factor;
	unsigned int powersave_bias;
	unsigned int io_is_busy;
} dbs_tuners_ins = {
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
	.down_differential = DEF_FREQUENCY_DOWN_DIFFERENTIAL,
	.ignore_nice = 0,
	.powersave_bias = 0,
};

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
							cputime64_t *wall)
{
	cputime64_t idle_time;
	cputime64_t cur_wall_time;
	cputime64_t busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
	busy_time = cputime64_add(kstat_cpu(cpu).cpustat.user,
			kstat_cpu(cpu).cpustat.system);

	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.irq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.softirq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.steal);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.nice);

	idle_time = cputime64_sub(cur_wall_time, busy_time);
	if (wall)
		*wall = (cputime64_t)jiffies_to_usecs(cur_wall_time);

	return (cputime64_t)jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);

	return idle_time;
}

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu, cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

/*
 * Find right freq to be set now with powersave_bias on.
 * Returns the freq_hi to be used right now and will set freq_hi_jiffies,
 * freq_lo, and freq_lo_jiffies in percpu area for averaging freqs.
 */
static unsigned int powersave_bias_target(struct cpufreq_policy *policy,
					  unsigned int freq_next,
					  unsigned int relation)
{
	unsigned int freq_req, freq_reduc, freq_avg;
	unsigned int freq_hi, freq_lo;
	unsigned int index = 0;
	unsigned int jiffies_total, jiffies_hi, jiffies_lo;
	struct cpu_dbs_info_s *dbs_info = &per_cpu(od_cpu_dbs_info,
						   policy->cpu);

	if (!dbs_info->freq_table) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_next;
	}

	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_next,
			relation, &index);
	freq_req = dbs_info->freq_table[index].frequency;
	freq_reduc = freq_req * dbs_tuners_ins.powersave_bias / 1000;
	freq_avg = freq_req - freq_reduc;

	/* Find freq bounds for freq_avg in freq_table */
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_H, &index);
	freq_lo = dbs_info->freq_table[index].frequency;
	index = 0;
	cpufreq_frequency_table_target(policy, dbs_info->freq_table, freq_avg,
			CPUFREQ_RELATION_L, &index);
	freq_hi = dbs_info->freq_table[index].frequency;

	/* Find out how long we have to be in hi and lo freqs */
	if (freq_hi == freq_lo) {
		dbs_info->freq_lo = 0;
		dbs_info->freq_lo_jiffies = 0;
		return freq_lo;
	}
	jiffies_total = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	jiffies_hi = (freq_avg - freq_lo) * jiffies_total;
	jiffies_hi += ((freq_hi - freq_lo) / 2);
	jiffies_hi /= (freq_hi - freq_lo);
	jiffies_lo = jiffies_total - jiffies_hi;
	dbs_info->freq_lo = freq_lo;
	dbs_info->freq_lo_jiffies = jiffies_lo;
	dbs_info->freq_hi_jiffies = jiffies_hi;
	return freq_hi;
}

static void intellidemand_powersave_bias_init_cpu(int cpu)
{
	struct cpu_dbs_info_s *dbs_info = &per_cpu(od_cpu_dbs_info, cpu);
	dbs_info->freq_table = cpufreq_frequency_get_table(cpu);
	dbs_info->freq_lo = 0;
}

static void intellidemand_powersave_bias_init(void)
{
	int i;
	for_each_online_cpu(i) {
		intellidemand_powersave_bias_init_cpu(i);
	}
}

/************************** sysfs interface ************************/

static ssize_t show_sampling_rate_max(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	printk_once(KERN_INFO "CPUFREQ: intellidemand sampling_rate_max "
	       "sysfs file is deprecated - used by: %s\n", current->comm);
	return sprintf(buf, "%u\n", -1U);
}

static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", min_sampling_rate);
}

define_one_global_ro(sampling_rate_max);
define_one_global_ro(sampling_rate_min);

/* cpufreq_intellidemand Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)              \
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(io_is_busy, io_is_busy);
show_one(up_threshold, up_threshold);
show_one(down_differential, down_differential);
show_one(sampling_down_factor, sampling_down_factor);
show_one(ignore_nice_load, ignore_nice);
show_one(powersave_bias, powersave_bias);

#ifdef CONFIG_SEC_LIMIT_MAX_FREQ // limit max freq
void set_lmf_active_load(unsigned long freq);
void set_lmf_inactive_load(unsigned long freq);
unsigned long get_lmf_active_load(void);
unsigned long get_lmf_inactive_load(void);

static ssize_t show_lmf_active_load(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", get_lmf_active_load());
}

static ssize_t show_lmf_inactive_load(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", get_lmf_inactive_load());
}
#endif

/*** delete after deprecation time ***/

#define DEPRECATION_MSG(file_name)					\
	printk_once(KERN_INFO "CPUFREQ: Per core ondemand sysfs "	\
		    "interface is deprecated - " #file_name "\n");

#define show_one_old(file_name)						\
static ssize_t show_##file_name##_old					\
(struct cpufreq_policy *unused, char *buf)				\
{									\
	printk_once(KERN_INFO "CPUFREQ: Per core ondemand sysfs "	\
		    "interface is deprecated - " #file_name "\n");	\
	return show_##file_name(NULL, NULL, buf);			\
}
show_one_old(sampling_rate);
show_one_old(up_threshold);
show_one_old(ignore_nice_load);
show_one_old(powersave_bias);
show_one_old(sampling_rate_min);
show_one_old(sampling_rate_max);

cpufreq_freq_attr_ro_old(sampling_rate_min);
cpufreq_freq_attr_ro_old(sampling_rate_max);

/*** delete after deprecation time ***/

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.sampling_rate = max(input, min_sampling_rate);
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.io_is_busy = !!input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_FREQUENCY_UP_THRESHOLD ||
			input < MIN_FREQUENCY_UP_THRESHOLD) {
		return -EINVAL;
	}

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.up_threshold = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_down_differential(struct kobject *a, struct attribute *b,
		const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	mutex_lock(&dbs_mutex);
	if (ret != 1 || input >= dbs_tuners_ins.up_threshold ||
			input < MIN_FREQUENCY_DOWN_DIFFERENTIAL) {
		mutex_unlock(&dbs_mutex);
		return -EINVAL;
	}

	dbs_tuners_ins.down_differential = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_sampling_down_factor(struct kobject *a,
			struct attribute *b, const char *buf, size_t count)
{
	unsigned int input, j;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;
	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.sampling_down_factor = input;

	/* Reset down sampling multiplier in case it was active */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(od_cpu_dbs_info, j);
		dbs_info->rate_mult = 1;
	}
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	mutex_lock(&dbs_mutex);
	if (input == dbs_tuners_ins.ignore_nice) { /* nothing to do */
		mutex_unlock(&dbs_mutex);
		return count;
	}
	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(od_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;

	}
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_powersave_bias(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 1000)
		input = 1000;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.powersave_bias = input;
	intellidemand_powersave_bias_init();
	mutex_unlock(&dbs_mutex);

	return count;
}

#ifdef CONFIG_SEC_LIMIT_MAX_FREQ // limit max freq
static ssize_t store_lmf_active_load(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned long input;
	int ret;

	ret = sscanf(buf, "%ld", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	set_lmf_active_load(input);
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_lmf_inactive_load(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned long input;
	int ret;

	ret = sscanf(buf, "%ld", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	set_lmf_inactive_load(input);
	mutex_unlock(&dbs_mutex);

	return count;
}
#endif

define_one_global_rw(sampling_rate);
define_one_global_rw(io_is_busy);
define_one_global_rw(up_threshold);
define_one_global_rw(down_differential);
define_one_global_rw(sampling_down_factor);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(powersave_bias);
#ifdef CONFIG_SEC_LIMIT_MAX_FREQ // limit max freq
define_one_global_rw(lmf_active_load);
define_one_global_rw(lmf_inactive_load);
#endif
static struct attribute *dbs_attributes[] = {
	&sampling_rate_max.attr,
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&up_threshold.attr,
	&down_differential.attr,
	&sampling_down_factor.attr,
	&ignore_nice_load.attr,
	&powersave_bias.attr,
	&io_is_busy.attr,
#ifdef CONFIG_SEC_LIMIT_MAX_FREQ // limit max freq
	&lmf_active_load.attr,
	&lmf_inactive_load.attr,
#endif
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "intellidemand",
};

/*** delete after deprecation time ***/

#define write_one_old(file_name)					\
static ssize_t store_##file_name##_old					\
(struct cpufreq_policy *unused, const char *buf, size_t count)		\
{									\
       printk_once(KERN_INFO "CPUFREQ: Per core ondemand sysfs "	\
		   "interface is deprecated - " #file_name "\n");	\
       return store_##file_name(NULL, NULL, buf, count);		\
}
write_one_old(sampling_rate);
write_one_old(up_threshold);
write_one_old(ignore_nice_load);
write_one_old(powersave_bias);

cpufreq_freq_attr_rw_old(sampling_rate);
cpufreq_freq_attr_rw_old(up_threshold);
cpufreq_freq_attr_rw_old(ignore_nice_load);
cpufreq_freq_attr_rw_old(powersave_bias);

static struct attribute *dbs_attributes_old[] = {
       &sampling_rate_max_old.attr,
       &sampling_rate_min_old.attr,
       &sampling_rate_old.attr,
       &up_threshold_old.attr,
       &ignore_nice_load_old.attr,
       &powersave_bias_old.attr,
       NULL
};

static struct attribute_group dbs_attr_group_old = {
       .attrs = dbs_attributes_old,
       .name = "intellidemand",
};

/*** delete after deprecation time ***/

/************************** sysfs end ************************/

static void dbs_freq_increase(struct cpufreq_policy *p, unsigned int freq)
{
	if (dbs_tuners_ins.powersave_bias)
		freq = powersave_bias_target(p, freq, CPUFREQ_RELATION_H);
	else if (p->cur == p->max)
		return;

	__cpufreq_driver_target(p, freq, dbs_tuners_ins.powersave_bias ?
			CPUFREQ_RELATION_L : CPUFREQ_RELATION_H);
}

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int max_load_freq;

	struct cpufreq_policy *policy;
	unsigned int j;

	this_dbs_info->freq_lo = 0;
	policy = this_dbs_info->cur_policy;

	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * Every sampling_rate, we look for a the lowest
	 * frequency which can sustain the load while keeping idle time over
	 * 30%. If such a frequency exist, we try to decrease to this frequency.
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of current frequency
	 */

	/* Get Absolute Load - in terms of freq */
	max_load_freq = 0;

	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info_s *j_dbs_info;
		cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;
		unsigned int load, load_freq;
		int freq_avg;

		j_dbs_info = &per_cpu(od_cpu_dbs_info, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);
		cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);

		wall_time = (unsigned int) cputime64_sub(cur_wall_time,
				j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int) cputime64_sub(cur_idle_time,
				j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		iowait_time = (unsigned int) cputime64_sub(cur_iowait_time,
				j_dbs_info->prev_cpu_iowait);
		j_dbs_info->prev_cpu_iowait = cur_iowait_time;

		if (dbs_tuners_ins.ignore_nice) {
			cputime64_t cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = cputime64_sub(kstat_cpu(j).cpustat.nice,
					 j_dbs_info->prev_cpu_nice);
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		/*
		 * For the purpose of ondemand, waiting for disk IO is an
		 * indication that you're performance critical, and not that
		 * the system is actually idle. So subtract the iowait time
		 * from the cpu idle time.
		 */

		if (dbs_tuners_ins.io_is_busy && idle_time >= iowait_time)
			idle_time -= iowait_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;

		freq_avg = __cpufreq_driver_getavg(policy, j);
		if (freq_avg <= 0)
			freq_avg = policy->cur;

		load_freq = load * freq_avg;
		if (load_freq > max_load_freq)
			max_load_freq = load_freq;
	}

	/* Check for frequency increase */
	if (max_load_freq > dbs_tuners_ins.up_threshold * policy->cur) {

/* In case of increase to max freq., freq. scales by 2 step for reducing the current consumption*/
#ifdef _LIMIT_LCD_OFF_CPU_MAX_FREQ_
		if(!cpufreq_gov_lcd_status) {
			if (policy->cur < policy->max) {
				if (policy->cur < 500000) dbs_freq_increase(policy, 800000);
				else if (policy->cur < 800000) dbs_freq_increase(policy, 1000000);
				else {
					this_dbs_info->rate_mult = dbs_tuners_ins.sampling_down_factor;
					dbs_freq_increase(policy, policy->max);
				}
			}
			return;
		} else
#endif
		/* If switching to max speed, apply sampling_down_factor */
		if (policy->cur < policy->max)
			this_dbs_info->rate_mult =
				dbs_tuners_ins.sampling_down_factor;
		dbs_freq_increase(policy, policy->max);
		return;
	}

	/* Check for frequency decrease */
	/* if we cannot reduce the frequency anymore, break out early */
	if (policy->cur == policy->min)
		return;

	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy. To be safe, we focus 10 points under the threshold.
	 */
	if (max_load_freq <
	    (dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential) *
	     policy->cur) {
		unsigned int freq_next;
		freq_next = max_load_freq /
				(dbs_tuners_ins.up_threshold -
				 dbs_tuners_ins.down_differential);

		/* No longer fully busy, reset rate_mult */
		this_dbs_info->rate_mult = 1;

		if (freq_next < policy->min)
			freq_next = policy->min;

		if (!dbs_tuners_ins.powersave_bias) {
			__cpufreq_driver_target(policy, freq_next,
					CPUFREQ_RELATION_L);
		} else {
			int freq = powersave_bias_target(policy, freq_next,
					CPUFREQ_RELATION_L);
			__cpufreq_driver_target(policy, freq,
				CPUFREQ_RELATION_L);
		}
	}
}


#ifdef CONFIG_SEC_LIMIT_MAX_FREQ // limit max freq

#include "../../kernel/power/power.h"

enum {	
	SET_MIN = 0,	
	SET_MAX
};

enum {	
	BOOT_CPU = 0,	
	NON_BOOT_CPU
};

#define SAMPLE_DURATION_MSEC	(10*1000) // 10 secs >= 10000 msec
#define ACTIVE_DURATION_MSEC	(30*1000) // 0.5 mins
#define INACTIVE_DURATION_MSEC	(1*60*1000) // 1 mins
#define MAX_ACTIVE_FREQ_LIMIT	95 // %
#define MAX_INACTIVE_FREQ_LIMIT	85 // %
#define ACTIVE_MAX_FREQ			1000000
#define INACTIVE_MAX_FREQ		200000

#define NUM_ACTIVE_LOAD_ARRAY	(ACTIVE_DURATION_MSEC/SAMPLE_DURATION_MSEC)
#define NUM_INACTIVE_LOAD_ARRAY	(INACTIVE_DURATION_MSEC/SAMPLE_DURATION_MSEC)
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

static unsigned long lmf_active_load_limit = MAX_ACTIVE_FREQ_LIMIT;
static unsigned long lmf_inactive_load_limit = MAX_INACTIVE_FREQ_LIMIT;

static unsigned long jiffies_old = 0;
static unsigned long time_int = 0;
static unsigned long load_state_total0  = 0;
static unsigned long load_limit_index = 0;	
static unsigned long load_limit_total[MAX(NUM_ACTIVE_LOAD_ARRAY,NUM_INACTIVE_LOAD_ARRAY)];
static unsigned long msecs_limit_total = 0;
static bool active_state = true;
static bool lmf_old_state = false;

extern int cpufreq_set_limits(int cpu, unsigned int limit, unsigned int value);
extern suspend_state_t get_suspend_state(void);

void set_lmf_active_load(unsigned long freq)
{
	lmf_active_load_limit = freq;
}

void set_lmf_inactive_load(unsigned long freq)
{
	lmf_inactive_load_limit = freq;
}

unsigned long get_lmf_active_load(void)
{
	return lmf_active_load_limit;
}

unsigned long get_lmf_inactive_load(void)
{
	return lmf_inactive_load_limit;
}
#endif

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;
	int sample_type = dbs_info->sample_type;

	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate
		* dbs_info->rate_mult);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

#ifdef CONFIG_SEC_LIMIT_MAX_FREQ // limit max freq

#ifdef CONFIG_HAS_EARLYSUSPEND
#ifdef _LIMIT_LCD_OFF_CPU_MAX_FREQ_
	if (cpufreq_gov_lcd_status)
#else
	if (!(get_suspend_state()==PM_SUSPEND_ON))
#endif
#else
	if (false)
#endif
	{
		if (lmf_old_state == true)
		{
			printk("LMF: disabled\n");
			lmf_old_state = false;
		}

		if (!active_state)
		{
			/* set freq to 1.0GHz */
			printk("LMF: CPU0 set max freq to 1.0GHz\n");
			cpufreq_set_limits(BOOT_CPU, SET_MAX, ACTIVE_MAX_FREQ);
		}
		
		jiffies_old = 0;
		time_int = 0;
		load_state_total0 = 0;
		msecs_limit_total = 0;
		load_limit_index = 0;
		active_state = true;
	}
	else // suspended or screen off
	{
		struct cpufreq_policy *policy;
		unsigned long load_state_cpu = 0;
		unsigned int delay_msec = 0;
		unsigned long jiffies_cur = jiffies;
		
		if (lmf_old_state == false)
		{
			printk("LMF: enabled\n");
			lmf_old_state = true;
		}

		if (jiffies_old == 0) 
		{
			jiffies_old = jiffies_cur;
		}
		else
		{
			delay_msec = jiffies_to_msecs(jiffies_cur - jiffies_old);
			jiffies_old = jiffies_cur;
			policy = dbs_info->cur_policy;
			load_state_cpu = ((policy->cur) * delay_msec)/10000;
			
			time_int += delay_msec;
			load_state_total0 += load_state_cpu;			
			
			/* average */
			if (time_int >= SAMPLE_DURATION_MSEC)
			{
				int i = 0;
				unsigned long ave_max = 0;
				unsigned long average = 0;
				unsigned long average_dec = 0;
				unsigned long total_load = 0;

				ave_max = (time_int / 10) * (ACTIVE_MAX_FREQ/1000);
				average = (load_state_total0 * 100) / ave_max;
				average_dec = (load_state_total0  * 100) % ave_max;

				msecs_limit_total += time_int;
				load_limit_total[load_limit_index++] = average;

				printk("LMF: average = %ld.%ld, (%ld) (%ld) (%ld:%ld)\n", 
					average, average_dec, time_int, load_state_total0, load_limit_index-1, msecs_limit_total);

				time_int = 0;
				load_state_total0 = 0;

				/* active */
				if (active_state)
				{
					if (load_limit_index >= NUM_ACTIVE_LOAD_ARRAY)
					{
						load_limit_index = 0;
					}
					
					if (msecs_limit_total > ACTIVE_DURATION_MSEC)
					{
						for (i=0; i<NUM_ACTIVE_LOAD_ARRAY; i++)
						{
							total_load += load_limit_total[i];
						}

						average = total_load / NUM_ACTIVE_LOAD_ARRAY;
						average_dec = total_load % NUM_ACTIVE_LOAD_ARRAY;
						printk("LMF:ACTIVE: total_avg = %ld.%ld\n", average, average_dec);

						if (average < lmf_active_load_limit)
						{
							msecs_limit_total = 0;
							load_limit_index = 0;
							active_state = false;

							/* set freq to 200MHz */
							printk("LMF: CPU0 set max freq to 200MHz\n");
							cpufreq_set_limits(BOOT_CPU, SET_MAX, INACTIVE_MAX_FREQ);
						}
						else
						{
							msecs_limit_total = ACTIVE_DURATION_MSEC; // to prevent overflow
						}
					}
				}
				else /* inactive */
				{
					if (load_limit_index >= NUM_INACTIVE_LOAD_ARRAY)
					{
						load_limit_index = 0;
					}
					
					if (msecs_limit_total > INACTIVE_DURATION_MSEC)
					{
						for (i=0; i<NUM_INACTIVE_LOAD_ARRAY; i++)
						{
							total_load += load_limit_total[i];
						}

						average = total_load / NUM_INACTIVE_LOAD_ARRAY;
						average_dec = total_load % NUM_INACTIVE_LOAD_ARRAY;
						printk("LMF:INACTIVE: total_avg = %ld.%ld\n", average, average_dec);

						if (average > lmf_inactive_load_limit)
						{
							msecs_limit_total = 0;
							load_limit_index = 0;
							active_state = true;

							/* set freq to 1.0GHz */
							printk("LMF: CPU0 set max freq to 1.0GHz\n");
							cpufreq_set_limits(BOOT_CPU, SET_MAX, ACTIVE_MAX_FREQ);
						}
						else
						{
							msecs_limit_total = INACTIVE_DURATION_MSEC; // to prevent overflow
						}
					}
				}
			}
		}
	}
#endif

	mutex_lock(&dbs_info->timer_mutex);

	/* Common NORMAL_SAMPLE setup */
	dbs_info->sample_type = DBS_NORMAL_SAMPLE;
	if (!dbs_tuners_ins.powersave_bias ||
	    sample_type == DBS_NORMAL_SAMPLE) {
		dbs_check_cpu(dbs_info);
		if (dbs_info->freq_lo) {
			/* Setup timer for SUB_SAMPLE */
			dbs_info->sample_type = DBS_SUB_SAMPLE;
			delay = dbs_info->freq_hi_jiffies;
		}
	} else {
		__cpufreq_driver_target(dbs_info->cur_policy,
			dbs_info->freq_lo, CPUFREQ_RELATION_H);
	}
	queue_delayed_work_on(cpu, kintellidemand_wq, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	dbs_info->sample_type = DBS_NORMAL_SAMPLE;
	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	queue_delayed_work_on(dbs_info->cpu, kintellidemand_wq, &dbs_info->work,
		delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	cancel_delayed_work_sync(&dbs_info->work);
}

/*
 * Not all CPUs want IO time to be accounted as busy; this dependson how
 * efficient idling at a higher frequency/voltage is.
 * Pavel Machek says this is not so for various generations of AMD and old
 * Intel systems.
 * Mike Chan (androidlcom) calis this is also not true for ARM.
 * Because of this, whitelist specific known (series) of CPUs by default, and
 * leave all others up to the user.
 */
static int should_io_be_busy(void)
{
#if defined(CONFIG_X86)
	/*
	 * For Intel, Core 2 (model 15) andl later have an efficient idle.
	 */
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL &&
	    boot_cpu_data.x86 == 6 &&
	    boot_cpu_data.x86_model >= 15)
		return 1;
#endif
#if defined(CONFIG_ARM)
	return 1;
#endif
	return 0;
}

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(od_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_mutex);

		rc = sysfs_create_group(&policy->kobj, &dbs_attr_group_old);
		if (rc) {
			mutex_unlock(&dbs_mutex);
			return rc;
		}

		dbs_enable++;
		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(od_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kstat_cpu(j).cpustat.nice;
			}
		}
		this_dbs_info->cpu = cpu;
		this_dbs_info->rate_mult = 1;
		intellidemand_powersave_bias_init_cpu(cpu);
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			unsigned int latency;

			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}

			/* policy latency is in nS. Convert it to uS first */
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;
			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate,
					MIN_LATENCY_MULTIPLIER * latency);
			//HTC: use QCT setting in /system/etc/init.post_boot.rc
			if (!dbs_tuners_ins.sampling_rate)
				dbs_tuners_ins.sampling_rate =
					max(min_sampling_rate,
					    latency * LATENCY_MULTIPLIER);
			dbs_tuners_ins.io_is_busy = should_io_be_busy();
		}
		mutex_unlock(&dbs_mutex);

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_timer_init(this_dbs_info);
		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		sysfs_remove_group(&policy->kobj, &dbs_attr_group_old);
		mutex_destroy(&this_dbs_info->timer_mutex);
		dbs_enable--;
		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);
		break;
	}
	return 0;
}

#ifdef _LIMIT_LCD_OFF_CPU_MAX_FREQ_
#ifdef CONFIG_HAS_EARLYSUSPEND
static void cpufreq_gov_suspend(struct early_suspend *h)
{
	cpufreq_gov_lcd_status = 0;

	pr_info("%s : cpufreq_gov_lcd_status %d\n", __func__, cpufreq_gov_lcd_status);
}

static void cpufreq_gov_resume(struct early_suspend *h)
{
	cpufreq_gov_lcd_status = 1;

	pr_info("%s : cpufreq_gov_lcd_status %d\n", __func__, cpufreq_gov_lcd_status);
}
#endif
#endif

static int __init cpufreq_gov_dbs_init(void)
{
	int err;
	cputime64_t wall;
	u64 idle_time;
	int cpu = get_cpu();

	idle_time = get_cpu_idle_time_us(cpu, &wall);
	put_cpu();
	if (idle_time != -1ULL) {
		/* Idle micro accounting is supported. Use finer thresholds */
		dbs_tuners_ins.up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
		dbs_tuners_ins.down_differential =
					MICRO_FREQUENCY_DOWN_DIFFERENTIAL;
		/*
		 * In no_hz/micro accounting case we set the minimum frequency
		 * not depending on HZ, but fixed (very low). The deferred
		 * timer might skip some samples if idle/sleeping as needed.
		*/
		min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
	} else {
		/* For correct statistics, we need 10 ticks for each measure */
		min_sampling_rate =
			MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10);
	}

	kintellidemand_wq = create_workqueue("kintellidemand");
	if (!kintellidemand_wq) {
		printk(KERN_ERR "Creation of kintellidemand failed\n");
		return -EFAULT;
	}
	err = cpufreq_register_governor(&cpufreq_gov_intellidemand);
	if (err)
		destroy_workqueue(kintellidemand_wq);

#ifdef _LIMIT_LCD_OFF_CPU_MAX_FREQ_
#ifdef CONFIG_HAS_EARLYSUSPEND
	cpufreq_gov_lcd_status = 1;

	cpufreq_gov_early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;

	cpufreq_gov_early_suspend.suspend = cpufreq_gov_suspend;
	cpufreq_gov_early_suspend.resume = cpufreq_gov_resume;
	register_early_suspend(&cpufreq_gov_early_suspend);
#endif
#endif

	return err;
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_intellidemand);
	destroy_workqueue(kintellidemand_wq);
}


MODULE_AUTHOR("Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>");
MODULE_AUTHOR("Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>");
MODULE_DESCRIPTION("'cpufreq_intellidemand' - A dynamic cpufreq governor for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_INTELLIDEMAND
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
