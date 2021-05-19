1\. We use the Google Bazel build system to compile the userspace components of
ghOSt. Go to `https://docs.bazel.build/versions/master/install.html` for
instructions to install Bazel on your operating system.

2\. Install ghOSt dependencies:

```sudo apt update
sudo apt install libnuma-dev libcap-dev```

3\. The experiment scripts in experiments/scripts/ use Python 3.7+. If you do
not have 3.7+ installed, then install it.

Furthermore, if running `python3` does not use 3.7+ by default, you either need
to change it to use 3.7+ by default, or if that is not possible, you need to
change the command that subpar uses to invoke Python. Once bazel grabs subpar
and all other dependencies in step 5, return here and do the following:

`vim bazel-userspace/external/subpar/compiler/cli.py`

Find `/usr/bin/env python3` and replace it with `/usr/bin/env python3.7` (or
whatever version you want to use.

Now redo step 5.

5\. Compile the ghOSt userspace component. Run the following from the root of the repository:

`bazel build -c opt ...`

`-c opt` tells Bazel to build the targets with optimizations turned on. `:all` tells Bazel to build all targets in the `BUILD` file, including the core ghOSt library, the schedulers, the unit tests, the experiments, and the scripts to run the experiments, along with all of the dependencies for those targets. If you prefer to build individual targets rather than all of them to save compile time, replace `:all` with an individual target name, such as `agent_shinjuku`.
