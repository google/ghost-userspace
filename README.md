# ghOSt: Fast &amp; Flexible User-Space Delegation of Linux Scheduling

ghOSt is a general-purpose delegation of scheduling policy implemented on top of
the Linux kernel. The ghOSt framework provides a rich API that receives
scheduling decisions for processes from userspace and actuates them as
transactions. Programmers can use any language or tools to develop policies,
which can be upgraded without a machine reboot. ghOSt supports policies for a
range of scheduling objectives, from µs-scale latency, to throughput, to energy
efficiency, and beyond, and incurs low overheads for scheduling actions. Many
policies are just a few hundred lines of code. Overall, ghOSt provides a
performant framework for delegation of thread scheduling policy to userspace
processes that enables policy optimization, non-disruptive upgrades, and fault
isolation.

[SOSP '21 Paper](https://dl.acm.org/doi/10.1145/3477132.3483542)\
[SOSP '21 Talk](https://youtu.be/j4ABe4dsbIY)

The ghOSt kernel is [here](https://www.github.com/google/ghost-kernel). You must
compile and run the userspace component on the ghOSt kernel.

This is not an officially supported Google product.

---

### Compilation

The ghOSt userspace component can be compiled on Ubuntu 20.04 or newer.

1\. We use the Google Bazel build system to compile the userspace components of
ghOSt. Go to the
[Bazel Installation Guide](https://docs.bazel.build/versions/main/install.html)
for instructions to install Bazel on your operating system.

2\. Install ghOSt dependencies:

```
sudo apt update
sudo apt install libnuma-dev libcap-dev libelf-dev libbfd-dev gcc clang-12 llvm zlib1g-dev python-is-python3
```

Note that ghOSt requires GCC 9 or newer and Clang 12 or newer.

3\. Compile the ghOSt userspace component. Run the following from the root of
the repository:

```
bazel build -c opt ...
```

`-c opt` tells Bazel to build the targets with optimizations turned on. `...`
tells Bazel to build all targets in the `BUILD` file and all `BUILD` files in
subdirectories, including the core ghOSt library, the eBPF code, the schedulers,
the unit tests, the experiments, and the scripts to run the experiments, along
with all of the dependencies for those targets. If you prefer to build
individual targets rather than all of them to save compile time, replace `...`
with an individual target name, such as `agent_shinjuku`.

---

### ghOSt Project Layout

- `bpf/user/`
  - ghOSt contains a suite of BPF tools to assist with debugging and performance
    optimization. The userspace components of these tools are in this directory.
- `experiments/`
  - The RocksDB and antagonist Shinjuku experiments (from our SOSP paper) and
    microbenchmarks. Use the Python scripts in `experiments/scripts/` to run the
    Shinjuku experiments.
- `kernel/`
  - Headers that have shared data structures used by both the kernel and
    userspace.
- `lib/`
  - The core ghOSt userspace library.
- `schedulers/`
  - ghOSt schedulers. These schedulers include:
    - `biff/`, Biff (bare-bones FIFO scheduler that schedules everything with
      BPF code)
    - `cfs/` CFS (ghOSt implementation of Linux Completely Fair Scheduler
      policy)
    - `edf/`, EDF (Earliest Deadline First)
    - `fifo/centralized/`, Centralized FIFO
    - `fifo/per_cpu/`, Per-CPU FIFO
    - `shinjuku/`, Shinjuku
    - `sol/`, Speed-of-Light (bare-bones centralized FIFO scheduler that runs as
      fast as possible)
- `shared/`
  - Classes to support shared-memory communication between a scheduler and
    another application(s). Generally, this communication is useful for the
    application to send scheduling hints to the scheduler.
- `tests/`
  - ghOSt unit tests.
- `third_party/`
  - `bpf/`
    - Contains the kernel BPF code for our suite of BPF tools (mentioned above).
      This kernel BPF code is licensed under GPLv2, so we must keep it in
      `third_party/`.
  - The rest of `third_party/` contains code from third-party developers and
    `BUILD` files to compile the code.
- `util/`
  -  Helper utilities for ghOSt. For example, `pushtosched` can be used to move
     a batch of kernel threads to the ghOSt scheduling class.
