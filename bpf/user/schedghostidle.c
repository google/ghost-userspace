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

#include "bpf/user/schedghostidle_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#define handle_error(msg) \
        do { perror(msg); exit(-1); } while (0)

/* Keep this in sync with schedghostidle.bpf.c. */
#define NR_SLOTS 25

static uint64_t start_time_ns, print_time_ns;

static void print_hist(int fd)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	unsigned int hist[NR_SLOTS] = {0};
	uint64_t *count;
	uint64_t total = 0;
	float total_sec;

	count = calloc(nr_cpus, sizeof(*count));
	if (!count)
		handle_error("calloc");

	for (int i = 0; i < NR_SLOTS; i++) {
		if (bpf_map_lookup_elem(fd, &i, count))
			handle_error("lookup");
		hist[i] = 0;
		for (int c = 0; c < nr_cpus; c++) {
			hist[i] += count[c];
			total += count[c];
		}
	}
	free(count);

	printf("\n");
	printf("Latency of a CPU going Idle until a task is Latched:\n");
	printf("----------------------------------------------------\n");
	print_log2_hist(hist, NR_SLOTS, "usec");

	total_sec = 1.0 * (print_time_ns - start_time_ns) / NSEC_PER_SEC;
	printf("\nTotal: %lu events over %f seconds (%f / sec) on %u cpus\n\n",
	       total, total_sec, total / total_sec, nr_cpus);
}

static volatile bool exiting;

static void sig_hand(int signr)
{
	exiting = true;
}

static struct sigaction sigact = {.sa_handler = sig_hand};

int main(int argc, char **argv)
{
	struct schedghostidle_bpf *obj;
	int err;

	sigaction(SIGINT, &sigact, 0);
	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return -1;
	}

	obj = schedghostidle_bpf__open_and_load();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return -1;
	}

	err = schedghostidle_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	start_time_ns = get_ktime_ns();

	printf("Ctrl-c to exit\n");

	while (!exiting)
		sleep(9999999);

	print_time_ns = get_ktime_ns();
	print_hist(bpf_map__fd(obj->maps.hist));

	printf("Total latches: %lu, bpf_latches %lu (%f), idle_to_bpf_latches %lu (%f)\n\n",
	       obj->bss->nr_latches,
	       obj->bss->nr_bpf_latches,
	       100.0 * obj->bss->nr_bpf_latches / obj->bss->nr_latches,
	       obj->bss->nr_idle_to_bpf_latches,
	       100.0 * obj->bss->nr_idle_to_bpf_latches / obj->bss->nr_latches);

cleanup:
	schedghostidle_bpf__destroy(obj);

	return 0;
}
