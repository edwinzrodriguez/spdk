# KV CMD

Tool to run individual KV store/retrieve/exist/delete/list commands.

Starting nvmf target with:

```bash
  sudo build/bin/nvmf_tgt -c examples/kv/local_kv.json -s 256
```

Run the kv\_cmd utility from another shell with:

Store:
```
   python -c "print('test')" | sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' \
   -d 256 -C -K aaaaaaaaaaaaaaaa -c store
```

Retrieve:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -K aaaaaaaaaaaaaaaa -c retrieve
```

Exist:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -K aaaaaaaaaaaaaaaa -c exist
```

Delete:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -K aaaaaaaaaaaaaaaa -c delete
```

List:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -K aaaaaaaaaaaaaaaa -c list
```
