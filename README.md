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

The ghOSt kernel is [here](https://www.github.com/google/ghost-kernel). You must
compile and run the userspace component on the ghOSt kernel.

This is not an officially supported Google product.

---

The ghOSt userspace component can be compiled on Ubuntu 20.04 or newer.

1\. We use the Google Bazel build system to compile the userspace components of
ghOSt. Go to the
[Bazel Installation Guide](https://docs.bazel.build/versions/main/install.html)
for instructions to install Bazel on your operating system.

2\. Install ghOSt dependencies:

```
sudo apt update
sudo apt install libnuma-dev libcap-dev libelf-dev libbfd-dev gcc clang llvm zlib1g-dev python-is-python3
```

Note that ghOSt requires GCC 9 or newer.

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
