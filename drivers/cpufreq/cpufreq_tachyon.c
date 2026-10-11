// SPDX-License-Identifier: GPL-2.0
/*
 *  drivers/cpufreq/cpufreq_tachyon.c
 *
 *  Tachyon - a spike-predicting cpufreq governor.
 *
 *  Named after Agnes Tachyon (Uma Musume: Pretty Derby): chase speed through
 *  calculation, ramp hard on a predicted spike, then decay fast once it ends.
 */

#include <linux/cpufreq.h>
#include <linux/math64.h>
#include <linux/slab.h>

#include "cpufreq_governor.h"

/* Compile-time predictor constants (not tunables). */
#define EWMA_SHIFT		2	/* smoothing factor 1/4 */
#define SPIKE_LOAD_FLOOR	30	/* %: a rising trend below this is noise */

/* Tunable starting defaults. */
#define DEF_SAMPLING_RATE		(10000)	/* µs, raised to framework min */
#define DEF_UP_THRESHOLD		(90)
#define DEF_RAMP_TREND_THRESHOLD	(25)
#define DEF_RAMP_TARGET_PCT		(80)
#define DEF_DECAY_STEP_PCT		(30)
#define DEF_HEADROOM_PCT		(25)

/* Tunable ranges. */
#define MIN_UP_THRESHOLD		(50)
#define MAX_UP_THRESHOLD		(100)
#define MIN_RAMP_TREND_THRESHOLD	(5)
#define MAX_RAMP_TREND_THRESHOLD	(100)
#define MIN_RAMP_TARGET_PCT		(1)
#define MAX_RAMP_TARGET_PCT		(100)
#define MIN_DECAY_STEP_PCT		(1)
#define MAX_DECAY_STEP_PCT		(100)
#define MIN_HEADROOM_PCT		(0)
#define MAX_HEADROOM_PCT		(100)

/* Private predictor state. Pure data operated on by tachyon_next_freq(). */
struct tachyon_pred {
	s32 ewma_fp;			/* smoothed load, 8 fractional bits */
	unsigned int boost_level;	/* 0 none, 1 ramped, 2 escalated */
	bool reset;			/* re-seed the predictor next sample */
};

/* Per-policy governor data. */
struct tachyon_policy_dbs_info {
	struct policy_dbs_info policy_dbs;
	struct tachyon_pred pred;
	u64 prev_sample_time;		/* framework clock of previous sample */
};

static inline struct tachyon_policy_dbs_info *
to_dbs_info(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct tachyon_policy_dbs_info,
			    policy_dbs);
}

/* Tunables that drive the decision function. */
struct tachyon_dbs_tuners {
	unsigned int ramp_trend_threshold;
	unsigned int ramp_target_pct;
	unsigned int decay_step_pct;
	unsigned int headroom_pct;
};

/*
 * tachyon_next_freq - the whole frequency decision, as a pure function.
 *
 * The only state it mutates is the predictor it is handed, so it can be lifted
 * verbatim into a userspace harness. No allocation, printk or sleeping here.
 */
static unsigned int tachyon_next_freq(struct tachyon_pred *pred,
				      const struct tachyon_dbs_tuners *tuners,
				      unsigned int up_threshold,
				      unsigned int load, unsigned int cur,
				      unsigned int pmin, unsigned int pmax,
				      unsigned int *relation)
{
	unsigned int target;
	int trend;
	bool saturated, rising;

	/* 1. Predictor (fixed-point EWMA, 8 fractional bits). */
	if (pred->reset) {
		pred->ewma_fp = (s32)load << 8;
		pred->reset = false;
		trend = 0;
	} else {
		int baseline = pred->ewma_fp >> 8;

		trend = (int)load - baseline;
		pred->ewma_fp += (((s32)load << 8) - pred->ewma_fp) >> EWMA_SHIFT;
	}

	/* 2. Classify. */
	saturated = load >= up_threshold;
	rising = trend >= (int)tuners->ramp_trend_threshold &&
		 load >= SPIKE_LOAD_FLOOR;

	/* 3. Boost level (two-stage ramp). */
	if (saturated)
		pred->boost_level = pred->boost_level ? 2 : 1;
	else if (rising)
		pred->boost_level = 1;
	else
		pred->boost_level = 0;

	/* 4. Target. */
	if (pred->boost_level == 2) {
		target = pmax;
		*relation = CPUFREQ_RELATION_H;
	} else if (pred->boost_level == 1) {
		unsigned int ramp = pmin + (unsigned int)
			div_u64((u64)(pmax - pmin) * tuners->ramp_target_pct,
				100);

		/* A ramp never lowers the frequency. */
		target = max(ramp, cur);
		*relation = CPUFREQ_RELATION_L;
	} else {
		/*
		 * Proportional demand with headroom; cur * load * (100 + hr)
		 * overflows 32 bits at multi-GHz, so compute in 64 bits.
		 */
		unsigned int prop = (unsigned int)
			div_u64((u64)cur * load * (100 + tuners->headroom_pct),
				10000);
		/* Geometric decay: a bounded fraction of the current freq. */
		unsigned int decay = cur - (unsigned int)
			div_u64((u64)cur * tuners->decay_step_pct, 100);

		/* Follow demand, but fall no faster than decay_step_pct. */
		target = max(prop, decay);
		*relation = CPUFREQ_RELATION_L;
	}

	/* 5. Clamp into the (possibly externally changed) policy range. */
	target = clamp_t(unsigned int, target, pmin, pmax);

	return target;
}

static unsigned int tachyon_dbs_update(struct cpufreq_policy *policy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct tachyon_policy_dbs_info *dbs_info = to_dbs_info(policy_dbs);
	struct dbs_data *dbs_data = policy_dbs->dbs_data;
	struct tachyon_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int load = dbs_update(policy);
	unsigned int sampling_rate = dbs_data->sampling_rate;
	unsigned int cur = policy->cur;
	unsigned int pmin = policy->min;
	unsigned int pmax = policy->max;
	unsigned int target, relation;
	u64 now = policy_dbs->last_sample_time;

	/*
	 * Stale gap: if far more than a sampling period elapsed (idle or NOHZ),
	 * a post-idle jump isn't a spike. Re-seed the predictor next sample.
	 */
	if (dbs_info->prev_sample_time &&
	    now - dbs_info->prev_sample_time > 2ULL * sampling_rate * NSEC_PER_USEC)
		dbs_info->pred.reset = true;
	dbs_info->prev_sample_time = now;

	target = tachyon_next_freq(&dbs_info->pred, tuners,
				   dbs_data->up_threshold, load, cur,
				   pmin, pmax, &relation);

	/* No-op avoidance: skip the driver call if the target won't move. */
	if (target != cur)
		__cpufreq_driver_target(policy, target, relation);

	return sampling_rate;
}

/************************** sysfs interface ************************/

static ssize_t up_threshold_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < MIN_UP_THRESHOLD || input > MAX_UP_THRESHOLD)
		return -EINVAL;

	dbs_data->up_threshold = input;
	return count;
}

static ssize_t ramp_trend_threshold_store(struct gov_attr_set *attr_set,
					  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct tachyon_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < MIN_RAMP_TREND_THRESHOLD ||
	    input > MAX_RAMP_TREND_THRESHOLD)
		return -EINVAL;

	tuners->ramp_trend_threshold = input;
	return count;
}

static ssize_t ramp_target_pct_store(struct gov_attr_set *attr_set,
				     const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct tachyon_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < MIN_RAMP_TARGET_PCT ||
	    input > MAX_RAMP_TARGET_PCT)
		return -EINVAL;

	tuners->ramp_target_pct = input;
	return count;
}

static ssize_t decay_step_pct_store(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct tachyon_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < MIN_DECAY_STEP_PCT ||
	    input > MAX_DECAY_STEP_PCT)
		return -EINVAL;

	tuners->decay_step_pct = input;
	return count;
}

static ssize_t headroom_pct_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);
	struct tachyon_dbs_tuners *tuners = dbs_data->tuners;
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < MIN_HEADROOM_PCT || input > MAX_HEADROOM_PCT)
		return -EINVAL;

	tuners->headroom_pct = input;
	return count;
}

gov_show_one_common(sampling_rate);
gov_show_one_common(up_threshold);
gov_show_one(tachyon, ramp_trend_threshold);
gov_show_one(tachyon, ramp_target_pct);
gov_show_one(tachyon, decay_step_pct);
gov_show_one(tachyon, headroom_pct);

gov_attr_rw(sampling_rate);
gov_attr_rw(up_threshold);
gov_attr_rw(ramp_trend_threshold);
gov_attr_rw(ramp_target_pct);
gov_attr_rw(decay_step_pct);
gov_attr_rw(headroom_pct);

static struct attribute *tachyon_attrs[] = {
	&sampling_rate.attr,
	&up_threshold.attr,
	&ramp_trend_threshold.attr,
	&ramp_target_pct.attr,
	&decay_step_pct.attr,
	&headroom_pct.attr,
	NULL
};
ATTRIBUTE_GROUPS(tachyon);

/************************** sysfs end ************************/

static struct policy_dbs_info *tachyon_alloc(void)
{
	struct tachyon_policy_dbs_info *dbs_info;

	dbs_info = kzalloc(sizeof(*dbs_info), GFP_KERNEL);
	return dbs_info ? &dbs_info->policy_dbs : NULL;
}

static void tachyon_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_dbs_info(policy_dbs));
}

static int tachyon_init(struct dbs_data *dbs_data)
{
	struct tachyon_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners)
		return -ENOMEM;

	tuners->ramp_trend_threshold = DEF_RAMP_TREND_THRESHOLD;
	tuners->ramp_target_pct = DEF_RAMP_TARGET_PCT;
	tuners->decay_step_pct = DEF_DECAY_STEP_PCT;
	tuners->headroom_pct = DEF_HEADROOM_PCT;

	dbs_data->up_threshold = DEF_UP_THRESHOLD;
	/* Framework clamps sampling_rate up to its own minimum later. */
	dbs_data->sampling_rate = max_t(unsigned int, dbs_data->sampling_rate,
					DEF_SAMPLING_RATE);
	dbs_data->tuners = tuners;

	return 0;
}

static void tachyon_exit(struct dbs_data *dbs_data)
{
	kfree(dbs_data->tuners);
}

static void tachyon_start(struct cpufreq_policy *policy)
{
	struct tachyon_policy_dbs_info *dbs_info =
		to_dbs_info(policy->governor_data);

	dbs_info->pred.reset = true;
	dbs_info->pred.boost_level = 0;
	dbs_info->pred.ewma_fp = 0;
	dbs_info->prev_sample_time = 0;
}

static struct dbs_governor tachyon_dbs_gov = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("tachyon"),
	.kobj_type = { .default_groups = tachyon_groups },
	.gov_dbs_update = tachyon_dbs_update,
	.alloc = tachyon_alloc,
	.free = tachyon_free,
	.init = tachyon_init,
	.exit = tachyon_exit,
	.start = tachyon_start,
};

#define CPU_FREQ_GOV_TACHYON	(tachyon_dbs_gov.gov)

MODULE_AUTHOR("iDead@rystX-OpenSource");
MODULE_DESCRIPTION("'cpufreq_tachyon' - a spike-predicting cpufreq governor");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_TACHYON
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &CPU_FREQ_GOV_TACHYON;
}
#endif

cpufreq_governor_init(CPU_FREQ_GOV_TACHYON);
cpufreq_governor_exit(CPU_FREQ_GOV_TACHYON);
