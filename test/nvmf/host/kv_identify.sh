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

timing_enter start_nvmf_tgt

"${NVMF_APP[@]}" -m 0xF &
nvmfpid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
timing_exit start_nvmf_tgt

$rpc_py bdev_kv_null_create KV0 $MALLOC_BDEV_SIZE
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
# NOTE: This will assign the same NGUID and EUI64 to all bdevs,
# but currently we only have one (see above), so this is OK.
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 KV0 \
	--nguid "ABCDEF0123456789ABCDEF0123456789" \
	--eui64 "ABCDEF0123456789"
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rpc_py nvmf_get_subsystems

$SPDK_EXAMPLE_DIR/identify -r "\
        trtype:$TEST_TRANSPORT \
        adrfam:IPv4 \
        traddr:$NVMF_FIRST_TARGET_IP \
        trsvcid:$NVMF_PORT \
        subnqn:nqn.2014-08.org.nvmexpress.discovery" -L all
$SPDK_EXAMPLE_DIR/identify -r "\
        trtype:$TEST_TRANSPORT \
        adrfam:IPv4 \
        traddr:$NVMF_FIRST_TARGET_IP \
        trsvcid:$NVMF_PORT \
        subnqn:nqn.2016-06.io.spdk:cnode1" -L all
sync
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
