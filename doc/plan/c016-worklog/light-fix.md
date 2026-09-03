| suite | pattern/transport | size | thr | lat | p95 | p99 |
|---|---|---|---|---|---|---|
| multi | MULTI_DEALER_ROUTER_REQREP/tcp | 1024 | 1.380 | 1.099 | 1.119 | 1.206 |
| multi | MULTI_DEALER_ROUTER_SENDSEND/tcp | 1024 | 1.072 | 1.725 | 1.756 | 1.594 |
| multi | MULTI_ROUTER_ROUTER_REQREP/tcp | 1024 | 1.057 | 1.137 | 1.142 | 1.157 |
| multi | MULTI_ROUTER_ROUTER_SENDSEND/tcp | 1024 | 0.897 | 1.306 | 1.387 | 1.412 |
| single | DEALER_ROUTER_REQREP/tcp | 1024 | 1.067 | 0.843 | 0.770 | 0.720 |
| single | ROUTER_ROUTER/inproc | 1024 | 1.235 | 0.567 | 0.810 | 0.826 |
| single | ROUTER_ROUTER/tcp | 1024 | 1.003 | 1.115 | 1.275 | 1.340 |
| single | ROUTER_ROUTER_REQREP/tcp | 1024 | 1.147 | 0.862 | 0.826 | 0.798 |
