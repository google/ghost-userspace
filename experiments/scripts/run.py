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
"""Runs RocksDB + Antagonist experiments.

Runs RocksDB + Antagonist experiments on both CFS (Linux Completely Fair
Scheduler) and ghOSt. Outputs the results to files.
"""
from __future__ import print_function
import datetime
import json
import os
import signal
import subprocess
from typing import List
from typing import Optional
from typing import TextIO
from dataclasses import asdict
from dataclasses import dataclass
from dataclasses import field
from experiments.scripts.options import MoveProcessToCgroup
from experiments.scripts.options import AntagonistOptions
from experiments.scripts.options import CreateCgroup
from experiments.scripts.options import DataClassToArgs
from experiments.scripts.options import DictToArgs
from experiments.scripts.options import GetBinaryPaths
from experiments.scripts.options import GetContainerArgs
from experiments.scripts.options import GetNiceArgs
from experiments.scripts.options import GhostOptions
from experiments.scripts.options import Paths
from experiments.scripts.options import RocksDBOptions
from experiments.scripts.setup import SetUp


@dataclass
class Experiment:
  """The experiment properties.

  Attributes:
    throughputs: A list of each throughput to run.
    output_prefix: The directory in which output files are stored. Each
      experiment creates another directory within this directory based on the
      current day and time.
    binaries: The paths to the binaries.
    rocksdb: The RocksDB options.
    antagonist: The Antagonist options, if the Antagonist should run. If not,
      set to `None`.
    ghost: The ghOSt options, if ghOSt should run. If not (because CFS is
      running), set to `None`.
  """
  throughputs: List[int] = field(default_factory=lambda: list)
  output_prefix: str = "/tmp/ghost_data"
  binaries: Paths = GetBinaryPaths()
  rocksdb: RocksDBOptions = RocksDBOptions()
  antagonist: Optional[AntagonistOptions] = None
  ghost: Optional[GhostOptions] = None


@dataclass
class AppHandles:
  """The application handles.

  Attributes:
    rocksdb: The RocksDB application handle.
    antagonist: The Antagonist application handle. Set to `None` when the
      Antagonist does not run in an experiment.
    ghost: The ghOSt application handle. Set to `None` when ghOSt does not run
      in an experiment because the experiment is scheduled by CFS (Linux
      Completely Fair Scheduler).
  """
  rocksdb: subprocess.Popen
  antagonist: Optional[subprocess.Popen]
  ghost: Optional[subprocess.Popen]


@dataclass
class OutputFiles:
  """The output file handles.

  Attributes:
    rocksdb: The main RocksDB output file (contains all results).
    rocksdb_get: The RocksDB output file that has the results for Get requests.
    rocksdb_range: The RocksDB output file that has the results for Range
      queries.
    antagonist: The Antagonist output file. Set to `None` when the Antagonist
      does not run in an experiment.
  """
  rocksdb: TextIO
  rocksdb_get: TextIO
  rocksdb_range: TextIO
  antagonist: Optional[TextIO] = None


def CheckBinaries(experiment: Experiment) -> bool:
  """Checks that all binaries need for the experiment exist.

  If the experiment does not require a binary (e.g., non-Antagonist
  experiments do not need the Antagonist binary), that binary is not checked.

  Args:
    experiment: The experiment.

  Returns:
    True if all of the binaries needed for the experiment exist. False
    otherwise.
  """
  rocksdb = os.path.exists(experiment.binaries.rocksdb)
  antagonist = not experiment.antagonist or os.path.exists(
      experiment.binaries.antagonist)
  ghost = not experiment.ghost or os.path.exists(experiment.binaries.ghost)
  return rocksdb and antagonist and ghost


def SetUpOutputDirectory(experiment: Experiment):
  """Creates the directory that output files are stored in.

  This output directory is created at `experiment.output_prefix`.

  The output directory itself is named after the current day and time.

  Example: /tmp/ghost_data/2021-01-06 14:17:15

  Args:
    experiment: The experiment.
  """
  if not os.path.exists(experiment.output_prefix):
    os.makedirs(experiment.output_prefix)

  date = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
  experiment.output_prefix = os.path.join(experiment.output_prefix, date)
  if os.path.exists(experiment.output_prefix):
    raise ValueError(
        f"The output directory {experiment.output_prefix} already exists.")
  os.makedirs(experiment.output_prefix)


def OpenOutputFiles(experiment: Experiment):
  """Opens all files that results are written to.

  These files include the RocksDB overall results file, the RocksDB Get
  request file, the RocksDB Range query file, and for Antagonist experiments,
  the Antagonist file.

  Args:
    experiment: The experiment.

  Returns:
    An instance of `OutputFiles` with filled handles.
  """
  rocksdb_prefix = os.path.join(experiment.output_prefix, "rocksdb")
  rocksdb = open(rocksdb_prefix, "w", encoding="ascii")
  rocksdb_get = open(rocksdb_prefix + "_get", "w", encoding="ascii")
  rocksdb_range = open(rocksdb_prefix + "_range", "w", encoding="ascii")
  outputs = OutputFiles(rocksdb, rocksdb_get, rocksdb_range)
  if experiment.antagonist:
    antagonist = os.path.join(experiment.output_prefix, "antagonist")
    outputs.antagonist = open(antagonist, "w", encoding="ascii")
  return outputs


def CloseOutputFiles(outputs: OutputFiles):
  """Closes all output files.

  Args:
    outputs: The files to close.
  """
  outputs.rocksdb.close()
  outputs.rocksdb_get.close()
  outputs.rocksdb_range.close()
  if outputs.antagonist:
    outputs.antagonist.close()


def GetAllOutputFiles(outputs: OutputFiles):
  """Returns a list of all output file handles.

  Args:
    outputs: The output file handles.

  Returns:
    A list of the output file handles.
  """
  ret = [outputs.rocksdb, outputs.rocksdb_get, outputs.rocksdb_range]
  if outputs.antagonist:
    ret += [outputs.antagonist]
  return ret


def DumpOptions(experiment: Experiment, outputs: OutputFiles):
  """Prints and saves all configuration options for the experiment.

  Args:
    experiment: The experiment.
    outputs: The output file handles.
  """
  write = json.dumps(asdict(experiment))
  print(write)

  for output in GetAllOutputFiles(outputs):
    output.write(f"{write}\n")
    output.flush()


def RocksDBArgs(experiment: Experiment, throughput: int, set_nice: bool):
  """Returns the RocksDB command line arguments for this experiment.

  Args:
    experiment: The experiment.
    throughput: The RocksDB throughput to generate in this experiment.
    set_nice: If True, prepends the argument list with a -20 nice value.

  Returns:
    A list of the command line arguments.
  """
  prefix_args = []
  if set_nice:
    # This is an experiment that runs the Antagonist on CFS (Linux Completely
    # Fair Scheduler).
    prefix_args = GetNiceArgs(-20)

  return prefix_args + [experiment.binaries.rocksdb] + DataClassToArgs(
      experiment.rocksdb) + DictToArgs({"throughput": str(throughput)})


def AntagonistArgs(experiment: Experiment, set_nice: bool):
  """Returns the Antagonist command line arguments for this experiment.

  Args:
    experiment: The experiment.
    set_nice: If True, prepends the argument list with a +19 nice value.

  Returns:
    A list of the command line arguments.
  """
  if not experiment.antagonist:
    raise ValueError("The Antagonist has not been configured.")

  prefix_args = []
  if set_nice:
    prefix_args = GetNiceArgs(19)

  return prefix_args + [experiment.binaries.antagonist] + DataClassToArgs(
      experiment.antagonist)


def GhostArgs(experiment: Experiment):
  """Returns the ghOSt command line arguments for this experiment.

  Args:
    experiment: The experiment.

  Returns:
    A list of the command line arguments.
  """
  if not experiment.ghost:
    raise ValueError("ghOSt has not been configured.")

  return [experiment.binaries.ghost] + DataClassToArgs(experiment.ghost)


def StartApp(args: List[str], cgroup: str):
  """Forks and execs an application with the command line arguments `args`.

  Creates a CPU cgroup and a memory cgroup with names `cgroup`. Moves the
  process to the created cgroup(s).

  Args:
    args: The command line arguments to start the application.
    cgroup: Creates a CPU cgroup and a memory cgroup with this name.

  Returns:
    The subprocess handle for the application.
  """
  CreateCgroup(cgroup)
  app = subprocess.Popen(args, stdout=subprocess.PIPE)
  print(args)
  MoveProcessToCgroup(cgroup, app.pid)
  return app


def WaitForLine(stream: TextIO, expected_line: str):
  """Waits until a line with contents `expected_line` is read from `stream`.

  This is used to wait for an application to initialize or for an
  application to print its results.

  Both of these actions are complete when an application prints a message
  indicating they are complete.

  Args:
    stream: The stream (generally the application's stdout).
    expected_line: The contents of the line that this function must read from
      `stream` before returning.
  """
  while True:
    line = stream.readline()
    line = line.decode("ascii").strip()
    if line == expected_line:
      return


def StartApps(experiment: Experiment, throughput: int):
  """Starts the applications for the experiment.

  Args:
    experiment: The experiment.
    throughput: The RocksDB throughput.

  Returns:
    The application handles.
  """
  # True if we set a nice value for RocksDB and (if it exists) the antagonist.
  set_nice: bool = not experiment.ghost and experiment.antagonist
  # Start ghOSt (if applicable).
  ghost = None
  if experiment.ghost:
    ghost = StartApp(GhostArgs(experiment), "ghost_shinjuku")
    # Wait for ghOSt to initialize.
    WaitForLine(ghost.stdout, "Initialization complete, ghOSt active.")

  # Start RocksDB.
  rocksdb = StartApp(RocksDBArgs(experiment, throughput, set_nice), "rocksdb")
  WaitForLine(rocksdb.stdout, "Initialization complete.")

  # Start the Antagonist (if applicable).
  antagonist = None
  if experiment.antagonist:
    antagonist = StartApp(AntagonistArgs(experiment, set_nice), "rocksdb")

  return AppHandles(rocksdb, antagonist, ghost)


def WaitForApps(handles: AppHandles):
  """Waits for the applications in the experiment to finish.

  This function first waits for the RocksDB application (and, if the
  Antagonist runs in this experiment, the Antagonist application) to finish.

  This app (or apps) finish on their own since they set a timer and exit
  when the timer fires.

  When this app (or apps) finish, the experiment is finished, so a SIGINT
  signal is sent to the ghOSt application since that application does not
  exit on its own. We must wait until RocksDB (and the Antagonist) exit before
  killing the ghOSt process since if the ghOSt process were to die before
  RocksDB (and the Antagonist), then the ghOSt threads in those applications
  will hang, causing the experiment to deadlock.

  Args:
    handles: The application handles.
  """
  handles.rocksdb.wait()
  if handles.antagonist:
    handles.antagonist.wait()
  if handles.ghost:
    handles.ghost.send_signal(signal.SIGINT)
    handles.ghost.wait()


def GetToStart(stream: TextIO):
  """Reads up to where the results are in the application's output stream.

  Args:
    stream: The application's output stream.
  """
  WaitForLine(stream, "Stats:")


def DoneWithResults(output: TextIO):
  """Final formatting for the results.

  Once the results of an experiment have been written to `output`, this function
  removes the last comma, adds a new line, and flushes the stream.

  Args:
    output: The stream that this function accesses.
  """
  # Remove the comma at the end. The file encoding is ASCII, so subtracting 1
  # will seek to right before the last character.
  output.seek(output.tell() - 1, os.SEEK_SET)
  output.truncate()

  output.write("\n")
  output.flush()


def HandleRocksDBOutput(stream: TextIO, outputs: OutputFiles):
  """Parse the RocksDB results from its output.

  Args:
    stream: The RocksDB application's output stream.
    outputs: The RocksDB files to write the results to.
  """

  print("RocksDB Stats:")
  output: Optional[TextIO] = None
  while True:
    line = stream.readline()
    if not line:
      if output != outputs.rocksdb:
        raise ValueError("Unexpected end of stream.")
      break
    line = line.decode("ascii").strip()
    print(line)

    if line == "Get:":
      if output:
        raise ValueError("Unexpected start of Get request results.")
      output = outputs.rocksdb_get
    elif line == "Range:":
      if output != outputs.rocksdb_get:
        raise ValueError("Unexpected start of Range query results.")
      output = outputs.rocksdb_range
    elif line == "All:":
      if output != outputs.rocksdb_range:
        raise ValueError("Unexpected start of All results.")
      output = outputs.rocksdb
    else:
      if not output:
        raise ValueError("No output stream to write results.")
      # This will add a comma at the end of the line. This comma is removed in
      # `DoneWithResults`.
      output.write(f"{line},")

  DoneWithResults(outputs.rocksdb_get)
  DoneWithResults(outputs.rocksdb_range)
  DoneWithResults(outputs.rocksdb)


def HandleAntagonistOutput(stream: TextIO, outputs: TextIO, throughput: int):
  """Writes the Antagonist results to the Antagonist output file.

  Args:
    stream: The Antagonist process's output stream that the results are read
      from.
    outputs: The output files that the results are written to.
    throughput: The RocksDB throughput. We include this in the output so that it
      is easier to match each Antagonist output line to its corresponding
      RocksDB output line.
  """
  print("Antagonist Stats:")
  outputs.antagonist.write(f"{throughput},")
  while True:
    line = stream.readline()
    if not line:
      break
    line = line.decode("ascii").strip()
    print(line)
    # This will add a comma at the end of the line. This comma is removed in
    # `DoneWithResults`.
    outputs.antagonist.write(f"{line},")

  DoneWithResults(outputs.antagonist)


def HandleOutput(handles: AppHandles, outputs: OutputFiles, throughput: int):
  """Writes the results to the output files.

  Writes the RocksDB results and (if relevant) the Antagonist results to the
  output files.

  Args:
    handles: The handles for the applications that ran during the experiments.
    outputs: The output files. Must include the RocksDB output files.
    throughput: The RocksDB throughput.
  """
  GetToStart(handles.rocksdb.stdout)
  HandleRocksDBOutput(handles.rocksdb.stdout, outputs)
  if handles.antagonist:
    GetToStart(handles.antagonist.stdout)
    HandleAntagonistOutput(handles.antagonist.stdout, outputs, throughput)


def RunExperiment(experiment: Experiment, outputs: OutputFiles,
                  throughput: int):
  """Runs the experiment and writes the results to the output files.

  Args:
    experiment: The experiment.
    outputs: The output files.
    throughput: The RocksDB throughput to generate in this experiment.
  """
  print(f"Running experiment for throughput = {throughput} req/s:")
  handles = StartApps(experiment, throughput)
  WaitForApps(handles)
  HandleOutput(handles, outputs, throughput)


def RunAllExperiments(experiment: Experiment, outputs: OutputFiles):
  """Runs all experiments and writes the results to the output files.

  There is one experiment per throughput in `experiment.throughputs`.

  Args:
    experiment: The experiment.
    outputs: The output files.
  """
  for throughput in experiment.throughputs:
    RunExperiment(experiment, outputs, throughput)


def Run(experiment: Experiment):
  """Run the experiment.

  Args:
    experiment: The experiment.
  """
  if experiment.ghost:
    print("Running ghOSt experiments...")
  else:
    print("Running CFS experiments...")

  SetUp(experiment.binaries)
  if not CheckBinaries(experiment):
    raise ValueError("One or more of the binaries does not exist.")
  SetUpOutputDirectory(experiment)
  print(f"Output Directory: {experiment.output_prefix}")
  outputs = OpenOutputFiles(experiment)
  DumpOptions(experiment, outputs)
  RunAllExperiments(experiment, outputs)
  CloseOutputFiles(outputs)
