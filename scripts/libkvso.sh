#!/bin/bash

SPDK_START_PATH=../
DPDK_LIB=dpdk/build/lib
SPDK_LIB=build/lib
KV_LIB=libkvnvmedd.a

cp $SPDK_START_PATH/$DPDK_LIB/librte_eal.a .
cp $SPDK_START_PATH/$DPDK_LIB/librte_telemetry.a .
cp $SPDK_START_PATH/$DPDK_LIB/librte_kvargs.a .
cp $SPDK_START_PATH/$DPDK_LIB/librte_mempool.a .
cp $SPDK_START_PATH/$DPDK_LIB/librte_ring.a .
cp $SPDK_START_PATH/$DPDK_LIB/librte_pci.a .
cp $SPDK_START_PATH/$DPDK_LIB/librte_bus_pci.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_env_dpdk.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_log.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_nvme.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_util.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_json.a .

cp $SPDK_START_PATH/$SPDK_LIB/libspdk_rdma.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_sock.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_sock_posix.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_jsonrpc.a .
cp $SPDK_START_PATH/$SPDK_LIB/libspdk_rpc.a .

ar -x librte_eal.a
ar -x librte_telemetry.a
ar -x librte_kvargs.a
ar -x librte_mempool.a
ar -x librte_ring.a
ar -x librte_pci.a
ar -x librte_bus_pci.a

ar -x libspdk_env_dpdk.a
ar -x libspdk_log.a
ar -x libspdk_nvme.a
ar -x libspdk_util.a
ar -x libspdk_json.a
ar -x libspdk_rdma.a
ar -x libspdk_sock.a
ar -x libspdk_sock_posix.a
ar -x libspdk_jsonrpc.a
ar -x libspdk_rpc.a

#ar -r $KV_LIB *.o
ar -r $KV_LIB ./*.o
mv $KV_LIB $SPDK_START_PATH/$SPDK_LIB/
rm -f librte_eal.a librte_telemetry.a librte_kvargs.a librte_mempool.a librte_ring.a librte_pci.a librte_bus_pci.a libspdk_env_dpdk.a libspdk_log.a libspdk_nvme.a libspdk_util.a libspdk_json.a libspdk_bdev_nvme.a libspdk_rdma.a libspdk_sock.a libspdk_sock_posix.a libspdk_jsonrpc.a libspdk_rpc.a
rm -f ./*.o
