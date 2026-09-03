| suite | pattern/transport | size | thr | lat | p95 | p99 |
|---|---|---|---|---|---|---|
| multi | MULTI_DEALER_ROUTER_REQREP/tcp | 1024 | 1.225 | 1.124 | 1.114 | 1.103 |
| multi | MULTI_DEALER_ROUTER_SENDSEND/tcp | 1024 | 1.200 | 3.154 | 4.366 | 3.930 |
| multi | MULTI_ROUTER_ROUTER_REQREP/tcp | 1024 | 0.963 | 1.201 | 1.247 | 1.234 |
| multi | MULTI_ROUTER_ROUTER_SENDSEND/tcp | 1024 | 1.001 | 1.014 | 0.957 | 0.986 |
| single | DEALER_ROUTER_REQREP/tcp | 1024 | 1.050 | 0.852 | 0.865 | 0.868 |
| single | ROUTER_ROUTER/inproc | 1024 | 1.196 | 0.522 | 0.849 | 0.618 |
| single | ROUTER_ROUTER/tcp | 1024 | 1.009 | 0.975 | 0.937 | 0.972 |
| single | ROUTER_ROUTER_REQREP/tcp | 1024 | 1.092 | 0.911 | 0.965 | 0.970 |
