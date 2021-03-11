#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 NetApp Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xF

$rootdir/scripts/gen_nvme.sh | $rpc_py load_subsystem_config

$rpc_py bdev_kv_null_create KV0 $MALLOC_BDEV_SIZE
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 KV0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$SPDK_EXAMPLE_DIR/perf -q 1 -o 4096 -w randrw -M 50 -t 1 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
$SPDK_EXAMPLE_DIR/perf -q 32 -o 4096 -w randrw -M 50 -t 1 -HI -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
$SPDK_EXAMPLE_DIR/perf -q 128 -o 131072 -w randrw -M 50 -t 2 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
sync
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
