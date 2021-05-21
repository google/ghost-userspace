1\. We use the Google Bazel build system to compile the userspace components of
ghOSt. Go to `https://docs.bazel.build/versions/master/install.html` for
instructions to install Bazel on your operating system.

2\. Install ghOSt dependencies:

```sudo apt update
sudo apt install libnuma-dev libcap-dev```

3\. We need to compile ghOSt with GCC 10, which is newer than the default version
of GCC included in Ubuntu 18.04 (GCC 7.5.0). Install GCC 10:

```sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install gcc-10
sudo apt install g++-10```

Ensure that `gcc-10 -v` indicates that GCC 10 is installed.

4\. Have Bazel fetch all dependencies:

`bazel build -c opt ...`

This command will fail. This is fine. We just want to fetch dependencies.

5\. We need to tell Bazel to use GCC 10.

`vim bazel-ghost-userspace/external/local_config_cc/BUILD`

For each path specified for `cxx_builtin_include_directories`, change all of the
GCC 7 paths to GCC 10 paths. For example, change
/usr/lib/gcc/x86_64-linux-gnu/7/include` to
`/usr/lib/gcc/x86_64-linux-gnu/10/include`.

Furthermore, in `tool_paths`, change `/usr/bin/gcc` to `/usr/bin/gcc-10`.

6\. The experiment scripts in experiments/scripts/ use Python 3.7+. If you do
not have 3.7+ installed, then install it.

Furthermore, if running `python3` does not use 3.7+ by default, you either need
to change it to use 3.7+ by default, or if that is not possible, you need to
change the command that subpar uses to invoke Python. Do the following:

`vim bazel-ghost-userspace/external/subpar/compiler/cli.py`

Find `/usr/bin/env python3` and replace it with `/usr/bin/env python3.7` (or
whatever version you want to use.

Now redo step 5.

7\. Compile the ghOSt userspace component. Run the following from the root of
the repository:

`bazel build -c opt ...`

`-c opt` tells Bazel to build the targets with optimizations turned on. `:all`
tells Bazel to build all targets in the `BUILD` file, including the core ghOSt
library, the schedulers, the unit tests, the experiments, and the scripts to run
the experiments, along with all of the dependencies for those targets. If you
prefer to build individual targets rather than all of them to save compile time,
replace `:all` with an individual target name, such as `agent_shinjuku`.
