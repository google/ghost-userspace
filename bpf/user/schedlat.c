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
#include <sys/param.h>
#include <unistd.h>

#include "third_party/bpf/schedlat.h"
#include "bpf/user/schedlat_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#define handle_error(msg) \
        do { perror(msg); exit(-1); } while (0)

static const char *titles[] = {
	[RUNNABLE_TO_LATCHED] = "Latency from Runnable to Latched",
	[LATCHED_TO_RUN] = "Latency from Latched to Run",
	[RUNNABLE_TO_RUN] = "Latency from Runnable to Run",
};

static void print_hists(int fd)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	struct hist *hist;
	uint32_t total[MAX_NR_HIST_SLOTS];

	/*
	 * There are NR_HISTS members of the PERCPU_ARRAY.  Each one we read is
	 * an *array[nr_cpus]* of the struct hist, one for each cpu.  This
	 * differs from a accessing an element from within a BPF program, where
	 * we only get the percpu element.
	 */
	hist = calloc(nr_cpus, sizeof(struct hist));
	if (!hist)
		handle_error("calloc");

	for (int i = 0; i < NR_HISTS; i++) {
		if (bpf_map_lookup_elem(fd, &i, hist))
			handle_error("lookup");
		memset(total, 0, sizeof(total));
		for (int c = 0; c < nr_cpus; c++) {
			for (int s = 0; s < MAX_NR_HIST_SLOTS; s++)
				total[s] += hist[c].slots[s];
		}
		printf("\n%s:\n----------\n", titles[i]);
		print_log2_hist(total, MAX_NR_HIST_SLOTS, "usec");
	}

	free(hist);
}

static volatile bool exiting;

static void sig_hand(int signr)
{
	exiting = true;
}

static struct sigaction sigact = {.sa_handler = sig_hand};

int main(int argc, char **argv)
{
	struct schedlat_bpf *obj;
	int err;

	sigaction(SIGINT, &sigact, 0);
	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return -1;
	}

	obj = schedlat_bpf__open_and_load();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return -1;
	}

	err = schedlat_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	printf("Ctrl-c to exit\n");
	while (!exiting)
		sleep(9999999);

	print_hists(bpf_map__fd(obj->maps.hists));

	printf("Exiting\n");

cleanup:
	schedlat_bpf__destroy(obj);

	return 0;
}
