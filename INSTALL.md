The ghOSt userspace component can be compiled on Ubuntu 20.04 or newer.

1\. We use the Google Bazel build system to compile the userspace components of
ghOSt. Go to the
[Bazel Installation Guide](https://docs.bazel.build/versions/main/install.html)
for instructions to install Bazel on your operating system.

2\. Install ghOSt dependencies:

```
sudo apt update
sudo apt install libnuma-dev libcap-dev libelf-dev libbfd-dev clang llvm zlib1g-dev python-is-python3
```

3\. Compile the ghOSt userspace component. Run the following from the root of
the repository:

`bazel build -c opt ...`

`-c opt` tells Bazel to build the targets with optimizations turned on. `...`
tells Bazel to build all targets in the `BUILD` file and all `BUILD` files in
subdirectories, including the core ghOSt library, the eBPF code, the schedulers,
the unit tests, the experiments, and the scripts to run the experiments, along
with all of the dependencies for those targets. If you prefer to build
individual targets rather than all of them to save compile time, replace `...`
with an individual target name, such as `agent_shinjuku`.
