| suite | pattern/transport | size | thr | lat | p95 | p99 |
|---|---|---|---|---|---|---|
| multi | MULTI_DEALER_ROUTER_REQREP/tcp | 1024 | 1.374 | 1.066 | 1.094 | 1.158 |
| multi | MULTI_DEALER_ROUTER_SENDSEND/tcp | 1024 | 0.984 | 1.521 | 1.549 | 1.660 |
| multi | MULTI_ROUTER_ROUTER_REQREP/tcp | 1024 | 1.116 | 1.229 | 1.234 | 1.229 |
| multi | MULTI_ROUTER_ROUTER_SENDSEND/tcp | 1024 | 1.022 | 1.039 | 0.998 | 0.954 |
| single | DEALER_ROUTER_REQREP/tcp | 1024 | 1.064 | 0.859 | 0.805 | 0.969 |
| single | ROUTER_ROUTER/inproc | 1024 | 1.236 | 0.485 | 0.287 | 0.677 |
| single | ROUTER_ROUTER/tcp | 1024 | 1.019 | 0.902 | 0.817 | 0.761 |
| single | ROUTER_ROUTER_REQREP/tcp | 1024 | 1.094 | 0.880 | 0.809 | 0.781 |
