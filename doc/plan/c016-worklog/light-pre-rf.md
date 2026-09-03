| suite | pattern/transport | size | thr | lat | p95 | p99 |
|---|---|---|---|---|---|---|
| multi | MULTI_DEALER_ROUTER_REQREP/tcp | 1024 | - | - | - | - |
| multi | MULTI_DEALER_ROUTER_SENDSEND/tcp | 1024 | 0.952 | 2.149 | 2.422 | 2.570 |
| multi | MULTI_ROUTER_ROUTER_REQREP/tcp | 1024 | 0.857 | 1.102 | 1.118 | 1.112 |
| multi | MULTI_ROUTER_ROUTER_SENDSEND/tcp | 1024 | 0.998 | 1.137 | 1.117 | 1.238 |
| single | DEALER_ROUTER_REQREP/tcp | 1024 | 1.035 | 0.838 | 0.712 | 0.816 |
| single | ROUTER_ROUTER/inproc | 1024 | 1.292 | 0.522 | 0.308 | 0.812 |
| single | ROUTER_ROUTER/tcp | 1024 | 0.937 | 0.963 | 0.868 | 0.913 |
| single | ROUTER_ROUTER_REQREP/tcp | 1024 | 1.157 | 0.808 | 0.761 | 0.787 |
