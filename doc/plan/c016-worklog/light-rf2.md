| suite | pattern/transport | size | thr | lat | p95 | p99 |
|---|---|---|---|---|---|---|
| multi | MULTI_DEALER_ROUTER_REQREP/tcp | 1024 | 1.285 | 1.100 | 1.100 | 1.114 |
| multi | MULTI_DEALER_ROUTER_SENDSEND/tcp | 1024 | 1.023 | 1.783 | 2.074 | 2.046 |
| multi | MULTI_ROUTER_ROUTER_REQREP/tcp | 1024 | 0.951 | 1.210 | 1.244 | 1.271 |
| multi | MULTI_ROUTER_ROUTER_SENDSEND/tcp | 1024 | 0.982 | 1.168 | 1.132 | 1.145 |
| single | DEALER_ROUTER_REQREP/tcp | 1024 | 1.081 | 0.918 | 1.036 | 1.199 |
| single | ROUTER_ROUTER/inproc | 1024 | 1.221 | 0.562 | 0.313 | 0.780 |
| single | ROUTER_ROUTER/tcp | 1024 | 1.034 | 1.021 | 1.350 | 1.099 |
| single | ROUTER_ROUTER_REQREP/tcp | 1024 | 1.060 | 0.923 | 0.981 | 1.001 |
