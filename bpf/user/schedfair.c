/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <unistd.h>

#include "third_party/bpf/schedfair.h"
#include "bpf/user/schedfair_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#define handle_error(msg) \
        do { perror(msg); exit(-1); } while (0)

static uint64_t scale99(uint64_t val, uint64_t max)
{
	uint64_t scale = val * 100 / max;

	if (val && !scale)
		scale = 1;
	scale = MIN(scale, 99);
	return scale;
}

/*
 * Prints a 40x20 matrix.  40 priorities (skips those with no data), and 20
 * buckets per priority row.
 *
 * Each row is normalized to row_max.  We could make it normalized to the global
 * max, perhaps controlled by a command line argument, though popular priorities
 * could dominate the graph such that you get all 1s (lowest non-zero value) for
 * the infrequent rows.
 */
static void print_percentiles(uint64_t *p_data)
{
	uint64_t table_max = 0;
	uint64_t table_total = 0;

	for (int i = 0; i < 40 * 20; i++) {
		table_max = MAX(p_data[i], table_max);
		table_total += p_data[i];
	}

	printf("Pr | .0 .1 .2 .3 .4 .5 .6 .7 .8 .9 1x 11 12 13 14 15 16 17 18 19 %%D row_max row_total\n");
	printf("---|---------------------------------------------------------------------------------\n");
	for (int prio = 0; prio < 40; prio++) {
		uint64_t *p = p_data + prio * 20;
		uint64_t row_max = 0;
		uint64_t row_total = 0;

		for (int perc = 0; perc < 20; perc++) {
			row_max = MAX(p[perc], row_max);
			row_total += p[perc];
		}
		if (!row_max)
			continue;

		printf("%2u |", prio);
		for (int perc = 0; perc < 20; perc++)
			printf(" %2lu", scale99(p[perc], row_max));
		printf(" %2.0f", 100.0 * row_total / table_total);
		printf(" %7lu", row_max);
		printf(" %9lu", row_total);
		printf("\n");
	}
}

static void print_wake_to_block_fairness(uint64_t *wtb_data)
{
	printf("\n");
	printf("Wake-to-block Fairness\n");
	printf("---------------------\n");
	printf("Each data point is a task's runtime / fairshare for each Wake-to-Block instance.\n");
	printf("\n");
	print_percentiles(wtb_data);
}

static void print_full_trace_fairness(int ti_fd)
{
	uint64_t *p_data;
	uint32_t pid;
	uint32_t lookup_key = 0;
	struct task_info ti[1];
	int ret;

	p_data = calloc(40 * 20, sizeof(uint64_t));
	assert(p_data);

	while (bpf_map_get_next_key(ti_fd, &lookup_key, &pid) == 0) {
		ret = bpf_map_lookup_elem(ti_fd, &pid, ti);
		lookup_key = pid;

		if (ret)
			continue;
		if (!ti->total_cpu_runtime || !ti->total_cpu_share)
			continue;

		/* Same calculation as in schedfair.bpf.c. */
		int fairness = MIN(19, 10 * ti->total_cpu_runtime
				       / ti->total_cpu_share);

		p_data[ti->user_prio * 20 + fairness]++;
	}

	printf("\n");
	printf("Full trace Fairness\n");
	printf("-----------------\n");
	printf("Each data point is a task's total_runtime / total_fair_share.\n");
	printf("\n");
	print_percentiles(p_data);

	free(p_data);
}

static volatile bool exiting;

static void sig_hand(int signr)
{
	exiting = true;
}

static struct sigaction sigact = {.sa_handler = sig_hand};

static size_t map_mmap_sz(struct bpf_map *map)
{
	const struct bpf_map_def *def = bpf_map__def(map);
	size_t mmap_sz;

	mmap_sz = (size_t)roundup(def->value_size, 8) * def->max_entries;
	mmap_sz = roundup(mmap_sz, sysconf(_SC_PAGE_SIZE));

	return mmap_sz;
}

void *bpf_map__mmap(struct bpf_map *map)
{
	return mmap(NULL, map_mmap_sz(map), PROT_READ | PROT_WRITE, MAP_SHARED,
		    bpf_map__fd(map), 0);
}

static void print_help_text(void)
{
	printf("- e.g. 1x = got your fair share.  .5 = got half your share.\n");
	printf("- Each row is a distribution of values for a given priority, where Pr 0 is nice -20\n");
	printf("- The row values are rounded down.  e.g. a task that got a fair share of [0.9, 1.0) will be in bin 9, with header .9\n");
	printf("- The row values are normalized within the row to [0,99], with the row_max value printed at the end.\n");
	printf("- The 'D' column is the percentage of density of values for a row out of the entire table: row_total / table_total\n");
	printf("- The row values differ for each type of trace.\n");
	printf("  - For Wake-to-Block, each value in the table represents the Fairness for each time a thread wakes, runs, and blocks.\n");
	printf("    This will happen many times for each thread.  The row_total column counts these run-to-blocks.\n");
	printf("  - For a Full Trace, each value in the table is single thread's fairness for the entire duration of the trace.\n");
	printf("    The row_total column counts threads.\n");
	printf("\n");
	printf("Example: this Wake-to-Block line:\n 1 | 42 60 65 62 57 53 51 50 50 50 50 51 53 55 58 61 61 58 50 99 61  176886   2030196\n");
	printf("- Had 2 million wake-run-block traces from all Pr1 threads.\n");
	printf("- The largest fairness bucket had 176K counts, normalized to '99'\n");
	printf("- That 99 was in the 19 column, which is 1.9x *or more*.\n");
	printf("- Mostly a uniform distribution, other than that 1.9x.\n");
	printf("\n");
	printf("Example: this Full Trace line:\n15 |  1  1  1  1  1  1 14  6  1  1  5 38 54 67 90 99 68 29  6  2 68     676      3314\n");
	printf("- Had 3313 threads of Pr15 (nice -5).\n");
	printf("- Had 676 threads in the largest fairness bucket (1.5x their fair share).\n");
	printf("- The distribution is nicely centered around 1.4 to 1.5, meaning these Pr15 tasks got more than their fair share.\n");
	printf("- This trace came from a workload of 68%% Pr15 and 28%% Pr20, so it makes sense that Pr15 got more (unweighted) share\n");

}

int main(int argc, char **argv)
{
	struct schedfair_bpf *obj;
	int err;
	uint64_t *wtb_data;
	uint64_t start_time, stop_time;

	sigaction(SIGINT, &sigact, 0);
	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return -1;
	}

	obj = schedfair_bpf__open();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return -1;
	}

	obj->rodata->nr_milli_cpus = 1000 * libbpf_num_possible_cpus();

	err = schedfair_bpf__load(obj);
	if (err) {
		fprintf(stderr, "failed to load BPF object\n");
		return -1;
	}

	wtb_data = bpf_map__mmap(obj->maps.wtb_fair_percentiles);
	if (wtb_data == MAP_FAILED) {
		fprintf(stderr, "failed to mmap\n");
		goto cleanup;
	}

	err = schedfair_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	printf("Measuring \"Task Fairness\": actual runtime / theoretical weighted fair share.\n");
	print_help_text();

	printf("Ctrl-c to exit\n");

	start_time = get_ktime_ns();
	while (!exiting)
		sleep(9999999);
	stop_time = get_ktime_ns();

	printf("\n");
	print_wake_to_block_fairness(wtb_data);
	printf("\n");
	print_full_trace_fairness(bpf_map__fd(obj->maps.tasks));
	printf("\n");

	printf("Traced for %.3f seconds\n", (1.0 * stop_time - start_time)
					    / 1000000000);

cleanup:
	schedfair_bpf__destroy(obj);

	return 0;
}
