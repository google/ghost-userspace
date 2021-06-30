// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/*
 * From iovisor's bcc/libbpf-tools/trace_helpers.{c,h}.
 *
 * If we wanted to use libbpf-tools' copy of this, we'd need a way to get
 * trace_helpers.o, which is created by libbpf-tools' Makefile.  It's designed
 * to be linked into program directly, but it's not a library that is exported.
 *
 * The tools including this header are similar in style to libbpf-tools, which
 * are intended to be built from within the bcc/libbpf-tools/ directory, using
 * that Makefile.  That would let us link against trace_helpers.o.  But since
 * we'd not building in libbpf-tools, it's simpler to have our own copy.
 */

#ifndef GHOST_LIB_BPF_TRACE_HELPERS_H_
#define GHOST_LIB_BPF_TRACE_HELPERS_H_

#include <sys/resource.h>
#include <time.h>
#include <stdio.h>

#define NSEC_PER_SEC		1000000000ULL

static inline unsigned long long get_ktime_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static inline int bump_memlock_rlimit(void)
{
	struct rlimit rlim_new = {
		.rlim_cur	= RLIM_INFINITY,
		.rlim_max	= RLIM_INFINITY,
	};

	return setrlimit(RLIMIT_MEMLOCK, &rlim_new);
}

static inline void print_stars(unsigned int val, unsigned int val_max, int width)
{
	int num_stars, num_spaces, i;
	bool need_plus;

	num_stars = (val < val_max ? val : val_max) * width / val_max;
	num_spaces = width - num_stars;
	need_plus = val > val_max;

	for (i = 0; i < num_stars; i++)
		printf("*");
	for (i = 0; i < num_spaces; i++)
		printf(" ");
	if (need_plus)
		printf("+");
}

static inline void print_log2_hist(unsigned int *vals, int vals_size,
			    const char *val_type)
{
	int stars_max = 40, idx_max = -1;
	unsigned int val, val_max = 0;
	unsigned long long low, high;
	int stars, width, i;

	for (i = 0; i < vals_size; i++) {
		val = vals[i];
		if (val > 0)
			idx_max = i;
		if (val > val_max)
			val_max = val;
	}

	if (idx_max < 0)
		return;

	printf("%*s%-*s : count    distribution\n", idx_max <= 32 ? 5 : 15, "",
		idx_max <= 32 ? 19 : 29, val_type);

	if (idx_max <= 32)
		stars = stars_max;
	else
		stars = stars_max / 2;

	for (i = 0; i <= idx_max; i++) {
		low = (1ULL << (i + 1)) >> 1;
		high = (1ULL << (i + 1)) - 1;
		if (low == high)
			low -= 1;
		val = vals[i];
		width = idx_max <= 32 ? 10 : 20;
		printf("%*lld -> %-*lld : %-8d |", width, low, width, high, val);
		print_stars(val, val_max, stars);
		printf("|\n");
	}
}

#endif  // GHOST_LIB_BPF_TRACE_HELPERS_H_
