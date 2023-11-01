#!/bin/bash

# If a scheduler crashes ungracefully, it can break future schedulers
# This script cleans up the ghOst enclaves
# https://github.com/google/ghost-userspace/issues/35

if [ "$EUID" -ne 0 ]
  then echo "Please run as root (sudo bash)"
  exit
fi

for i in /sys/fs/ghost/enclave_*/ctl; do echo destroy > $i; done
