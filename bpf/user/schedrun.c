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

#include "third_party/bpf/schedrun.h"
#include "bpf/user/schedrun_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#define error_exit(msg) do {	\
	perror(msg);		\
	exit(EXIT_FAILURE);	\
} while (0)

static bool ghost_only = false;
static pid_t pid = 0;

static const char *titles[] = {
	[RUNTIMES_PREEMPTED_YIELDED] = "Runtimes of preempted/yielded tasks",
	[RUNTIMES_BLOCKED] = "Runtimes of tasks that blocked",
	[RUNTIMES_ALL] = "All task runtimes",
};

// TODO: refactor (copied from schedlat.c).
static void print_hists(int fd)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	struct hist *hist;
	uint32_t total[MAX_NR_HIST_SLOTS];

	/*
	 * There are NR_HISTS members of the PERCPU_ARRAY.  Each one we read is
	 * an *array[nr_cpus]* of the struct hist, one for each cpu.  This
	 * differs from accessing an element from within a BPF program, where
	 * we only get the percpu element.
	 */
	hist = calloc(nr_cpus, sizeof(struct hist));
	if (!hist)
		error_exit("calloc");

	for (int i = 0; i < NR_HISTS; i++) {
		if (bpf_map_lookup_elem(fd, &i, hist))
			error_exit("bpf_map_lookup_elem");
		memset(total, 0, sizeof(total));
		for (int c = 0; c < nr_cpus; c++) {
			for (int s = 0; s < MAX_NR_HIST_SLOTS; s++)
				total[s] += hist[c].slots[s];
		}
		printf("\n%s:\n----------\n", titles[i]);
		print_log2_hist(total, MAX_NR_HIST_SLOTS, "usec");
	}
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

	print_hists(bpf_map__fd(skel->maps.hists));
	printf("Exiting\n");

cleanup:
	schedrun_bpf__destroy(skel);
	return -err;
}
