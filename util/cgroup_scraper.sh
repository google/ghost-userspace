#!/bin/bash
#
# Copyright 2022 Google LLC
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
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
