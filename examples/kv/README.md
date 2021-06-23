# KV key format for human consumption

```text
A key string passed, for example to kv_cmd can take one of 2 forms:
- An ascii string of up to 16 bytes in length, i.e. "hellokitty"
- A series of hex digits preceded by '0x' and grouped in to 4 byte sequences
  separated by '-'.  Note that the final group need not be all 4 bytes, so for
  example, '0x0' is a 1 byte key '0', 0x00 is also a 1 byte key with 0, while
  0x000 is a 2 byte key with bytes 0 and 1 set to 0.  Note also that key length
  is a distinguishing characteristic, so that 0x00 and 0x0000 are distinct keys

Output format for a key is similar, with only the significant bytes output as
determined by the key length.  For example, the key "hello" is output as:
"0x68656c6c-68"
while the key "hellokitty" will be represented as:
"0x68656c6c-68656c6c-6865"

```

## KV_NULL debugging Examples

After compiling the repository with make, start the nvmf target with examples:

```bash
  sudo build/bin/nvmf_tgt -s 256
```

```bash
  sudo scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
  sudo scripts/rpc.py bdev_kv_null_create KV0 20
  sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
  sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 KV0
  sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420
```

## JSON Config Save

Once you've configured your system, you can save the json configuration to a file with:

```bash
  sudo scripts/rpc.py save_config > ~/local_kv_bdev.json
```

Then pass the path to the resulting joson to nvmf_tgt with the '-c' option:

```bash
  sudo build/bin/nvmf_tgt -c ~/local_kv_bdev.json -s 256
```

## KV Identify

```bash
  sudo build/examples/identify -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256
```

## KV Perf

```bash
  sudo build/examples/perf -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420  subnqn:nqn.2016-06.io.spdk:cnode1' -o 4096 -w randrw -M 50 -t 10 -q 8 -s 256
```

End
