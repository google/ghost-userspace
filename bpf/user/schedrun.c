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

#include "bpf/user/schedrun_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#define error_exit(msg) do {	\
	perror(msg);		\
	exit(EXIT_FAILURE);	\
} while (0)

/* Keep this in sync with schedrun.bpf.c. */
#define NUM_BUCKETS 25

static bool ghost_only = false;
static pid_t pid = 0;

static void print_hist(int fd)
{
	uint64_t *counts;
	unsigned int hist[NUM_BUCKETS] = {0};

	int ncpus = libbpf_num_possible_cpus();
	if (ncpus < 0)
		error_exit("libbpf_num_possible_cpus");

	counts = calloc(ncpus, sizeof(*counts));
	if (!counts)
		error_exit("calloc");

	for (int i = 0; i < NUM_BUCKETS; ++i) {
		if (bpf_map_lookup_elem(fd, &i, counts))
			error_exit("bpf_map_lookup_elem");
		hist[i] = 0;
		for (int c = 0; c < ncpus; ++c)
			hist[i] += counts[c];
	}

	free(counts);

	printf("\n");
	printf("Task Runtime\n");
	printf("------------\n");
	print_log2_hist(hist, NUM_BUCKETS, "usec");
	printf("\n");
}

int main(int argc, char **argv)
{
	sigset_t set;
	int opt, err, sig;
	struct schedrun_bpf *skel;

	if (sigemptyset(&set))
		error_exit("sigemptyset");
	if (sigaddset(&set, SIGINT))
		error_exit("sigaddset");
	if (sigprocmask(SIG_BLOCK, &set, NULL))
		error_exit("sigprocmask");

	while ((opt = getopt(argc, argv, "gp:")) != -1) {
		switch (opt) {
			case 'g':
				ghost_only = true;
				break;
			case 'p':
				errno = 0;
				pid = strtol(optarg, NULL, 10);
				if (errno)
					error_exit("strtol");
				if (pid <= 0) {
					fprintf(stderr, "Invalid pid: %s\n", optarg);
					return 1;
				}
				break;
			default:
				fprintf(stderr, "Usage: %s [-p pid | -g]\n", argv[0]);
				return 1;
		}
	}

	if (ghost_only && pid) {
		fprintf(stderr, "-g and -p options are mutually exclusive\n");
		return 1;
	}

	if (bump_memlock_rlimit())
		error_exit("bump_memlock_rlimit");

	skel = schedrun_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}

	skel->rodata->ghost_only = ghost_only;
	skel->rodata->targ_tgid = pid;

	err = schedrun_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF skeleton\n");
		return 1;
	}

	err = schedrun_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF skeleton\n");
		goto cleanup;
	}

	printf("Ctrl-c to exit\n");

	if (sigwait(&set, &sig))
		error_exit("sigwait");

	print_hist(bpf_map__fd(skel->maps.hist));
	printf("Exiting\n");

cleanup:
	schedrun_bpf__destroy(skel);
	return -err;
}
