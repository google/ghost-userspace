#!/bin/bash
#
# Copyright 2022 Google LLC
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
#
# Moves tasks with the matching cgroup/cpu.ghost_enabled value to an enclave

if [ $# -lt 1 ]; then
        echo "Usage: ./`basename $0` ENCLAVE_DIR [GHOST_ENABLED_VAL]"
        exit -1
fi
ENCLAVE_DIR=$1

if [ $# -ge 2 ]; then
	ENABLED_VAL=$2
else
	# current kernels only support '1'.  In the future, we'll probably
	# allow a u32 that can be an enclave ID
	ENABLED_VAL=1
fi


declare -A enclave_tasks
for T in `cat $ENCLAVE_DIR/tasks`; do
	enclave_tasks[$T]="1"
done

for CGE in `find /dev/cgroup/cpu -name cpu.ghost_enabled`; do
	[[ "$ENABLED_VAL" != "`cat $CGE`" ]] && continue

	DIRNAME=`dirname $CGE`
	for T in `cat $DIRNAME/tasks`; do
		[[ ${enclave_tasks[$T]} ]] && continue

		echo $T > $ENCLAVE_DIR/tasks
	done
done
