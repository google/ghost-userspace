#!/bin/bash

# If a scheduler crashes ungracefully, it can break future schedulers
# This script cleans up the ghOst enclaves
# https://github.com/google/ghost-userspace/issues/35

sudo sh -c 'for i in /sys/fs/ghost/enclave_*/ctl; do echo destroy > $i; done'
