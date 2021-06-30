# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Sets up the machine for the experiments by disabling deep C-states and disabling profiling.

This script disables all C-states on all CPUs other than C-state 0, disables
cgroup profiling and socket profiling, mounts a tmpfs backed by hugepages, and
copies the experiment binaries included as data dependencies to the tmpfs mount.
"""

import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import zipfile
from experiments.scripts.options import Paths
from experiments.scripts.options import TMPFS_MOUNT


def DisableCStates():
  """Disables deep C-states."""
  # Loop through all CPUs
  cpu_prefix: str = "/sys/devices/system/cpu/cpu"
  cpu: int = 0
  while True:
    cpu_path: str = f"{cpu_prefix}{str(cpu)}"
    if not os.path.exists(cpu_path):
      break
    # Loop through all states
    state_prefix: str = os.path.join(cpu_path, "cpuidle/state")
    # Disable states 2 and greater. Leave states 0 and 1 enabled. Note that we
    # want to leave state 1 enabled because even though wakeup latency is
    # higher, we reduce power consumption which increases turbo frequency.
    state: int = 2
    while True:
      # Disable the state
      state_path: str = f"{state_prefix}{str(state)}"
      if os.path.exists(state_path):
        disable_path = os.path.join(state_path, "disable")
        disable_handle = open(disable_path, "w")
        disable_handle.write("1")
        disable_handle.close()
      else:
        break
      state += 1
    cpu += 1


def MountTmpfs():
  """Mounts a tmpfs backed with hugepages at `TMPFS_MOUNT`.

  The mount size is 10 GiB.
  """
  subprocess.run(["mount", "-o", "huge=always,remount,size=10G", TMPFS_MOUNT],
                 check=True)


def MountCgroups():
  """Mount the cgroup hierarchy."""
  os.system("mkdir -p /dev/cgroup/cpu")
  os.system("mkdir -p /dev/cgroup/memory")
  os.system("mount -t cgroup -o cpu,cpuacct cgroup /dev/cgroup/cpu")
  os.system("mount -t cgroup -o memory cgroup /dev/cgroup/memory")


def CopyBinary(copy_from: str, copy_to: str):
  """Copy a binary data dependency to an external path.

  This is used to copy a binary included as a data dependency at `copy_from` to
  a tmpfs mount backed by hugepages at `copy_to`.

  Note that this function is meant specifically for *binaries*. It sets the
  execute permission bit on the copied file, which should only be done for
  binaries, not other file types.

  Args:
    copy_from: The internal resource path for the data dependency.
    copy_to: The external path to copy the data dependency to.
  """
  # Copy the binary.
  shutil.copyfile(copy_from, copy_to)

  # Set the execute permission bit on the binary.
  st = os.stat(copy_to)
  os.chmod(copy_to, st.st_mode | stat.S_IEXEC)


def GetPar():
  """Returns the path to the PAR file.

  This is a bit of a hack since we know that this script is three levels down
  from the project root. Ideally, we would be able to use `pkgutil.get_data` to
  access the PAR file's data dependencies, but I cannot figure out how to get it
  working. https://github.com/google/subpar/issues/43

  Returns:
    The path to the PAR file.
  """
  return str(pathlib.Path(__file__).parents[3])


def UnzipPar():
  """Unzips the PAR file into a temporary directory.

  Returns:
    The temporary directory in which the PAR file was unzipped.
  """
  tmp = tempfile.TemporaryDirectory()
  path = tmp.name
  with zipfile.ZipFile(GetPar(), "r") as zf:
    zf.extractall(path)
  return tmp


def CopyBinaries(paths: Paths):
  """Copies RocksDB, Antagonist, and ghOSt binaries to external paths.

  The RocksDB, Antagonist, and ghOSt binaries are included as data dependencies
  in the `experiments` library (see the BUILD file). This function copies the
  RocksDB, Antagonist, and ghOSt binaries to the paths in `paths`.

  Args:
    paths: The paths to copy the RocksDB, Antagonist, and ghOSt binaries to.
  """
  tmp = UnzipPar()
  CopyBinary(tmp.name + "/com_google_ghost/rocksdb", paths.rocksdb)
  CopyBinary(tmp.name + "/com_google_ghost/antagonist", paths.antagonist)
  CopyBinary(tmp.name + "/com_google_ghost/agent_shinjuku", paths.ghost)
  tmp.cleanup()


# TODO: Disable MSV fixing.
def SetUp(binaries: Paths):
  """Set up the machine for the experiments.

  This includes disabling C-states, disabling profiling, mounting a tmpfs
  backed by hugepages, and copying the experiment binaries (included as data
  dependencies) to the tmpfs mount.

  Args:
    binaries: The paths to copy the binaries to.
  """
  DisableCStates()
  MountTmpfs()
  MountCgroups()
  CopyBinaries(binaries)
