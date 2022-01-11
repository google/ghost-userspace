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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>

#include "bpf/user/schedclasstop_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#define handle_error(msg) \
        do { perror(msg); exit(-1); } while (0)

#define SCHED_GHOST 18
#define SCHED_AGENT 19  /* Not a real sched class */
#define MAX_SCHED_CLASS (SCHED_AGENT + 1)

static struct sched_class {
	char *name;
	char symbol;
	uint64_t total;
} sched_class[MAX_SCHED_CLASS] = {
	[0]  = {"CFS",		'c'},
	[1]  = {"FIFO",		'f'},
	[2]  = {"RR",		'r'},
	[3]  = {"BATCH",	'b'},
	[4]  = {"IDLE",		'.'},	/* Idle task */
	[5]  = {"SCH_IDLE",	'i'},
	[6]  = {"DEADLINE",	'd'},
	[7]  = {"SCH_7",	'7'},
	[8]  = {"SCH_8",	'8'},
	[9]  = {"SCH_9",	'9'},
	[10] = {"SCH_10",	'A'},
	[11] = {"SCH_11",	'B'},
	[12] = {"SCH_12",	'C'},
	[13] = {"SCH_13",	'D'},
	[14] = {"SCH_14",	'E'},
	[15] = {"SCH_15",	'F'},
	[16] = {"SCH_16",	'X'},
	[17] = {"SCH_17",	'Y'},
	[18] = {"GHOST",	'g'},
	[19] = {"AGENT",	'a'},
};

static uint64_t start_time_ns;

static unsigned int get_nr_columns(void)
{
	struct winsize w;

	ioctl(0, TIOCGWINSZ, &w);
	return w.ws_col;
}

static void print_class_times(int fd)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	uint64_t *class_cpu_times;	/* matrix[class][cpu] */
	uint64_t max_time = 0;
	uint64_t print_time_ns;
	unsigned int width, ns_per_char;

	print_time_ns = get_ktime_ns();

	class_cpu_times = calloc(nr_cpus, sizeof(uint64_t) * MAX_SCHED_CLASS);
	if (!class_cpu_times)
		handle_error("calloc");

	for (int i = 0; i < MAX_SCHED_CLASS; i++) {
		/*
		 * Each lookup returns a uint64_t[nr_cpus] of the times for a given
		 * class for all cpus.
		 */
		if (bpf_map_lookup_elem(fd, &i, &class_cpu_times[i * nr_cpus]))
			handle_error("lookup");
	}

	for (int c = 0; c < nr_cpus; c++) {
		uint64_t cpu_total = 0;

		for (int i = 0; i < MAX_SCHED_CLASS; i++) {
			uint64_t *i_c = &class_cpu_times[i * nr_cpus + c];

			cpu_total += *i_c;
			sched_class[i].total += *i_c;
		}
		max_time = MAX(max_time, cpu_total);
	}

	/* 72 max, so we can copy/paste it into emails easily. */
	width = MIN(72, get_nr_columns());
	width -= 6;	/* 3 for cpu, 3 for ' | ' */
	ns_per_char = MAX(max_time / width, 1);
	printf("\nCPU\n");
	printf("----|---\n");
	for (int c = 0; c < nr_cpus; c++) {
		printf("%3u | ", c);
		for (int i = 0; i < MAX_SCHED_CLASS; i++) {
			uint64_t *i_c = &class_cpu_times[i * nr_cpus + c];
			unsigned int num_chars = *i_c / ns_per_char;

			for (int j = 0; j < num_chars; j++)
				printf("%c", sched_class[i].symbol);
		}
		printf("\n");
	}
	printf("\n");

	float total_sec = 1.0 * (print_time_ns - start_time_ns) / NSEC_PER_SEC;
	float total_cpu_sec = nr_cpus * total_sec;

	printf("Total: %u cpus over %f seconds: %f cpu-sec\n", nr_cpus,
	       total_sec, total_cpu_sec);
	printf("------------------------------------------------\n");
	for (int i = 0; i < MAX_SCHED_CLASS; i++) {
		if (sched_class[i].total) {
			float cpu_sec = 1.0 * sched_class[i].total /
				NSEC_PER_SEC;

			printf("%-10s (%c): %20f cpu-seconds (%f)\n",
			       sched_class[i].name, sched_class[i].symbol,
			       cpu_sec, cpu_sec / total_cpu_sec);
		}
	}
	printf("\n");
}

static volatile bool exiting;

static void sig_hand(int signr)
{
	exiting = true;
}

static struct sigaction sigact = {.sa_handler = sig_hand};

int main(int argc, char **argv)
{
	struct schedclasstop_bpf *obj;
	int err;

	sigaction(SIGINT, &sigact, 0);
	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return -1;
	}

	obj = schedclasstop_bpf__open_and_load();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return -1;
	}

	err = schedclasstop_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	start_time_ns = get_ktime_ns();

	printf("Ctrl-c to exit\n");

	while (!exiting)
		sleep(9999999);

	print_class_times(bpf_map__fd(obj->maps.class_times));

cleanup:
	schedclasstop_bpf__destroy(obj);

	return 0;
}
