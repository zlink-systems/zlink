## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |       15.09 KOPS |   15.45 MB/s |     0.064 ms |     0.081 ms |     0.113 ms |       1.9% |    15.9 MB |       2.5% |    14.8 MB |
      | zlink-cpp               | 1024B    |        9.37 KOPS |    9.59 MB/s |     0.106 ms |     0.125 ms |     0.182 ms |       2.0% |    16.8 MB |       1.5% |     7.4 MB |
      | grpc-cpp                | 4096B    |       14.93 KOPS |   61.15 MB/s |     0.065 ms |     0.083 ms |     0.123 ms |       1.9% |    17.1 MB |       2.5% |    14.9 MB |
      | zlink-cpp               | 4096B    |        9.34 KOPS |   38.25 MB/s |     0.106 ms |     0.127 ms |     0.176 ms |       2.0% |    17.1 MB |       1.6% |     7.6 MB |

  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |       66.54 KOPS |   68.13 MB/s |     1.502 ms |     1.666 ms |     1.760 ms |       3.6% |    20.7 MB |      27.0% |    38.4 MB |
      | zlink-cpp               | 1024B    |      325.65 KOPS |  333.46 MB/s |     0.307 ms |     0.374 ms |     0.592 ms |       8.2% |    21.2 MB |       7.6% |    32.1 MB |
      | grpc-cpp                | 4096B    |       64.08 KOPS |  262.48 MB/s |     1.559 ms |     1.756 ms |     1.913 ms |       3.7% |    21.3 MB |      27.1% |    46.8 MB |
      | zlink-cpp               | 4096B    |      291.56 KOPS | 1194.23 MB/s |     0.342 ms |     0.411 ms |     0.487 ms |       9.3% |    22.6 MB |       8.5% |    33.2 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |     44.70 KMSG/s |   45.77 MB/s |     0.114 ms |     0.169 ms |     0.215 ms |       3.2% |    22.6 MB |      10.9% |    46.9 MB |
      | zlink-cpp               | 1024B    |    693.57 KMSG/s |  710.22 MB/s |     0.919 ms |     1.103 ms |     1.189 ms |       7.9% |    22.6 MB |       5.5% |    52.9 MB |
      | grpc-cpp                | 4096B    |     44.17 KMSG/s |  180.91 MB/s |     0.115 ms |     0.169 ms |     0.213 ms |       3.2% |    22.6 MB |      10.9% |    47.0 MB |
      | zlink-cpp               | 4096B    |    422.20 KMSG/s | 1729.32 MB/s |     0.457 ms |     0.631 ms |     0.715 ms |       7.7% |    22.6 MB |       6.2% |    42.5 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-cpp-request-serial,local,1024,throughput,15092.296
RESULT,current,grpc-cpp-request-serial,local,1024,bandwidth,15.455
RESULT,current,grpc-cpp-request-serial,local,1024,latency,0.064
RESULT,current,grpc-cpp-request-serial,local,1024,latency_p95,0.081
RESULT,current,grpc-cpp-request-serial,local,1024,latency_p99,0.113
RESULT,current,grpc-cpp-request-serial,local,1024,client_cpu_percent,1.923
RESULT,current,grpc-cpp-request-serial,local,1024,client_memory_mb,15.863
RESULT,current,grpc-cpp-request-serial,local,1024,server_cpu_percent,2.454
RESULT,current,grpc-cpp-request-serial,local,1024,server_memory_mb,14.844
RESULT,current,zlink-cpp-request-serial,local,1024,throughput,9367.756
RESULT,current,zlink-cpp-request-serial,local,1024,bandwidth,9.593
RESULT,current,zlink-cpp-request-serial,local,1024,latency,0.106
RESULT,current,zlink-cpp-request-serial,local,1024,latency_p95,0.125
RESULT,current,zlink-cpp-request-serial,local,1024,latency_p99,0.182
RESULT,current,zlink-cpp-request-serial,local,1024,client_cpu_percent,2.019
RESULT,current,zlink-cpp-request-serial,local,1024,client_memory_mb,16.793
RESULT,current,zlink-cpp-request-serial,local,1024,server_cpu_percent,1.526
RESULT,current,zlink-cpp-request-serial,local,1024,server_memory_mb,7.410
RESULT,current,grpc-cpp-request-serial,local,4096,throughput,14929.827
RESULT,current,grpc-cpp-request-serial,local,4096,bandwidth,61.153
RESULT,current,grpc-cpp-request-serial,local,4096,latency,0.065
RESULT,current,grpc-cpp-request-serial,local,4096,latency_p95,0.083
RESULT,current,grpc-cpp-request-serial,local,4096,latency_p99,0.123
RESULT,current,grpc-cpp-request-serial,local,4096,client_cpu_percent,1.886
RESULT,current,grpc-cpp-request-serial,local,4096,client_memory_mb,17.105
RESULT,current,grpc-cpp-request-serial,local,4096,server_cpu_percent,2.532
RESULT,current,grpc-cpp-request-serial,local,4096,server_memory_mb,14.879
RESULT,current,zlink-cpp-request-serial,local,4096,throughput,9338.227
RESULT,current,zlink-cpp-request-serial,local,4096,bandwidth,38.249
RESULT,current,zlink-cpp-request-serial,local,4096,latency,0.106
RESULT,current,zlink-cpp-request-serial,local,4096,latency_p95,0.127
RESULT,current,zlink-cpp-request-serial,local,4096,latency_p99,0.176
RESULT,current,zlink-cpp-request-serial,local,4096,client_cpu_percent,1.982
RESULT,current,zlink-cpp-request-serial,local,4096,client_memory_mb,17.105
RESULT,current,zlink-cpp-request-serial,local,4096,server_cpu_percent,1.585
RESULT,current,zlink-cpp-request-serial,local,4096,server_memory_mb,7.590
RESULT,current,grpc-cpp-request-window,local,1024,throughput,66536.549
RESULT,current,grpc-cpp-request-window,local,1024,bandwidth,68.133
RESULT,current,grpc-cpp-request-window,local,1024,latency,1.502
RESULT,current,grpc-cpp-request-window,local,1024,latency_p95,1.666
RESULT,current,grpc-cpp-request-window,local,1024,latency_p99,1.760
RESULT,current,grpc-cpp-request-window,local,1024,client_cpu_percent,3.553
RESULT,current,grpc-cpp-request-window,local,1024,client_memory_mb,20.699
RESULT,current,grpc-cpp-request-window,local,1024,server_cpu_percent,27.027
RESULT,current,grpc-cpp-request-window,local,1024,server_memory_mb,38.449
RESULT,current,zlink-cpp-request-window,local,1024,throughput,325646.855
RESULT,current,zlink-cpp-request-window,local,1024,bandwidth,333.462
RESULT,current,zlink-cpp-request-window,local,1024,latency,0.307
RESULT,current,zlink-cpp-request-window,local,1024,latency_p95,0.374
RESULT,current,zlink-cpp-request-window,local,1024,latency_p99,0.592
RESULT,current,zlink-cpp-request-window,local,1024,client_cpu_percent,8.181
RESULT,current,zlink-cpp-request-window,local,1024,client_memory_mb,21.168
RESULT,current,zlink-cpp-request-window,local,1024,server_cpu_percent,7.605
RESULT,current,zlink-cpp-request-window,local,1024,server_memory_mb,32.113
RESULT,current,grpc-cpp-request-window,local,4096,throughput,64081.416
RESULT,current,grpc-cpp-request-window,local,4096,bandwidth,262.477
RESULT,current,grpc-cpp-request-window,local,4096,latency,1.559
RESULT,current,grpc-cpp-request-window,local,4096,latency_p95,1.756
RESULT,current,grpc-cpp-request-window,local,4096,latency_p99,1.913
RESULT,current,grpc-cpp-request-window,local,4096,client_cpu_percent,3.710
RESULT,current,grpc-cpp-request-window,local,4096,client_memory_mb,21.324
RESULT,current,grpc-cpp-request-window,local,4096,server_cpu_percent,27.102
RESULT,current,grpc-cpp-request-window,local,4096,server_memory_mb,46.816
RESULT,current,zlink-cpp-request-window,local,4096,throughput,291559.875
RESULT,current,zlink-cpp-request-window,local,4096,bandwidth,1194.229
RESULT,current,zlink-cpp-request-window,local,4096,latency,0.342
RESULT,current,zlink-cpp-request-window,local,4096,latency_p95,0.411
RESULT,current,zlink-cpp-request-window,local,4096,latency_p99,0.487
RESULT,current,zlink-cpp-request-window,local,4096,client_cpu_percent,9.262
RESULT,current,zlink-cpp-request-window,local,4096,client_memory_mb,22.574
RESULT,current,zlink-cpp-request-window,local,4096,server_cpu_percent,8.464
RESULT,current,zlink-cpp-request-window,local,4096,server_memory_mb,33.246
RESULT,current,grpc-cpp-send-saturation,local,1024,throughput,44698.909
RESULT,current,grpc-cpp-send-saturation,local,1024,bandwidth,45.772
RESULT,current,grpc-cpp-send-saturation,local,1024,latency,0.114
RESULT,current,grpc-cpp-send-saturation,local,1024,latency_p95,0.169
RESULT,current,grpc-cpp-send-saturation,local,1024,latency_p99,0.215
RESULT,current,grpc-cpp-send-saturation,local,1024,client_cpu_percent,3.160
RESULT,current,grpc-cpp-send-saturation,local,1024,client_memory_mb,22.574
RESULT,current,grpc-cpp-send-saturation,local,1024,server_cpu_percent,10.883
RESULT,current,grpc-cpp-send-saturation,local,1024,server_memory_mb,46.875
RESULT,current,zlink-cpp-send-saturation,local,1024,throughput,693570.124
RESULT,current,zlink-cpp-send-saturation,local,1024,bandwidth,710.216
RESULT,current,zlink-cpp-send-saturation,local,1024,latency,0.919
RESULT,current,zlink-cpp-send-saturation,local,1024,latency_p95,1.103
RESULT,current,zlink-cpp-send-saturation,local,1024,latency_p99,1.189
RESULT,current,zlink-cpp-send-saturation,local,1024,client_cpu_percent,7.897
RESULT,current,zlink-cpp-send-saturation,local,1024,client_memory_mb,22.574
RESULT,current,zlink-cpp-send-saturation,local,1024,server_cpu_percent,5.457
RESULT,current,zlink-cpp-send-saturation,local,1024,server_memory_mb,52.934
RESULT,current,grpc-cpp-send-saturation,local,4096,throughput,44168.202
RESULT,current,grpc-cpp-send-saturation,local,4096,bandwidth,180.913
RESULT,current,grpc-cpp-send-saturation,local,4096,latency,0.115
RESULT,current,grpc-cpp-send-saturation,local,4096,latency_p95,0.169
RESULT,current,grpc-cpp-send-saturation,local,4096,latency_p99,0.213
RESULT,current,grpc-cpp-send-saturation,local,4096,client_cpu_percent,3.235
RESULT,current,grpc-cpp-send-saturation,local,4096,client_memory_mb,22.574
RESULT,current,grpc-cpp-send-saturation,local,4096,server_cpu_percent,10.874
RESULT,current,grpc-cpp-send-saturation,local,4096,server_memory_mb,46.996
RESULT,current,zlink-cpp-send-saturation,local,4096,throughput,422197.635
RESULT,current,zlink-cpp-send-saturation,local,4096,bandwidth,1729.322
RESULT,current,zlink-cpp-send-saturation,local,4096,latency,0.457
RESULT,current,zlink-cpp-send-saturation,local,4096,latency_p95,0.631
RESULT,current,zlink-cpp-send-saturation,local,4096,latency_p99,0.715
RESULT,current,zlink-cpp-send-saturation,local,4096,client_cpu_percent,7.672
RESULT,current,zlink-cpp-send-saturation,local,4096,client_memory_mb,22.574
RESULT,current,zlink-cpp-send-saturation,local,4096,server_cpu_percent,6.229
RESULT,current,zlink-cpp-send-saturation,local,4096,server_memory_mb,42.484


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Client CPU% | Client cores | Client MB | Server CPU% | Server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 15.092 | 0.064 | 0.081 | 0.113 | 1.9 | 0.38 | 15.9 | 2.5 | 14.8 | 164 |
| request-serial | 1024 | `zlink-cpp` | 9.368 | 0.106 | 0.125 | 0.182 | 2.0 | 0.40 | 16.8 | 1.5 | 7.4 | 159 |
| request-serial | 4096 | `grpc-cpp` | 14.930 | 0.065 | 0.083 | 0.123 | 1.9 | 0.38 | 17.1 | 2.5 | 14.9 | 163 |
| request-serial | 4096 | `zlink-cpp` | 9.338 | 0.106 | 0.127 | 0.176 | 2.0 | 0.40 | 17.1 | 1.6 | 7.6 | 159 |
| request-window | 1024 | `grpc-cpp` | 66.537 | 1.502 | 1.666 | 1.760 | 3.6 | 0.71 | 20.7 | 27.0 | 38.4 | 216 |
| request-window | 1024 | `zlink-cpp` | 325.647 | 0.307 | 0.374 | 0.592 | 8.2 | 1.64 | 21.2 | 7.6 | 32.1 | 436 |
| request-window | 4096 | `grpc-cpp` | 64.081 | 1.559 | 1.756 | 1.913 | 3.7 | 0.74 | 21.3 | 27.1 | 46.8 | 212 |
| request-window | 4096 | `zlink-cpp` | 291.560 | 0.342 | 0.411 | 0.487 | 9.3 | 1.85 | 22.6 | 8.5 | 33.2 | 414 |
| send-saturation | 1024 | `grpc-cpp` | 44.699 | 0.114 | 0.169 | 0.215 | 3.2 | 0.63 | 22.6 | 10.9 | 46.9 | 194 |
| send-saturation | 1024 | `zlink-cpp` | 693.570 | 0.919 | 1.103 | 1.189 | 7.9 | 1.58 | 22.6 | 5.5 | 52.9 | 499 |
| send-saturation | 4096 | `grpc-cpp` | 44.168 | 0.115 | 0.169 | 0.213 | 3.2 | 0.65 | 22.6 | 10.9 | 47.0 | 193 |
| send-saturation | 4096 | `zlink-cpp` | 422.198 | 0.457 | 0.631 | 0.715 | 7.7 | 1.53 | 22.6 | 6.2 | 42.5 | 506 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | client cores | saturation metric | reading | declared ceiling | saturated | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | 0 | 1.0 | 164 | no | 0.38 | submit_thread_cores | 0.386 | 1 | no | — |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | 0 | 1.0 | 159 | no | 0.40 | submit_thread_cores | 0.184 | 1 | no | — |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | 0 | 1.0 | 163 | no | 0.38 | submit_thread_cores | 0.376 | 1 | no | — |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | 0 | 1.0 | 159 | no | 0.40 | submit_thread_cores | 0.172 | 1 | no | — |
| request-window | 1024 | `grpc-cpp` | 100 | 100 | 0 | 99.9 | 216 | no | 0.71 | submit_thread_cores | 0.712 | 1 | no | — |
| request-window | 1024 | `zlink-cpp` | 100 | 100 | 0 | 99.9 | 436 | no | 1.64 | submit_thread_cores | 0.942 | 1 | no | — |
| request-window | 4096 | `grpc-cpp` | 100 | 100 | 0 | 99.9 | 212 | no | 0.74 | submit_thread_cores | 0.742 | 1 | no | — |
| request-window | 4096 | `zlink-cpp` | 100 | 100 | 0 | 99.8 | 414 | no | 1.85 | submit_thread_cores | 0.936 | 1 | no | — |
| send-saturation | 1024 | `grpc-cpp` | 8 | 8 | 0 | 5.1 | 194 | no | 0.63 | submit_thread_cores | 0.632 | 1 | no | server |
| send-saturation | 1024 | `zlink-cpp` | 8 | 8 | 0 | 637.2 | 499 | no | 1.58 | submit_thread_cores | 0.858 | 1 | no | server |
| send-saturation | 4096 | `grpc-cpp` | 8 | 8 | 0 | 5.1 | 193 | no | 0.65 | submit_thread_cores | 0.646 | 1 | no | server |
| send-saturation | 4096 | `zlink-cpp` | 8 | 8 | 0 | 192.8 | 506 | no | 1.53 | submit_thread_cores | 0.652 | 1 | no | server |

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | n/a (1 run) | n/a |
| request-serial | 1024 | `zlink-cpp` | 1 | n/a (1 run) | n/a |
| request-serial | 4096 | `grpc-cpp` | 1 | n/a (1 run) | n/a |
| request-serial | 4096 | `zlink-cpp` | 1 | n/a (1 run) | n/a |
| request-window | 1024 | `grpc-cpp` | 1 | n/a (1 run) | n/a |
| request-window | 1024 | `zlink-cpp` | 1 | n/a (1 run) | n/a |
| request-window | 4096 | `grpc-cpp` | 1 | n/a (1 run) | n/a |
| request-window | 4096 | `zlink-cpp` | 1 | n/a (1 run) | n/a |
| send-saturation | 1024 | `grpc-cpp` | 1 | n/a (1 run) | n/a |
| send-saturation | 1024 | `zlink-cpp` | 1 | n/a (1 run) | n/a |
| send-saturation | 4096 | `grpc-cpp` | 1 | n/a (1 run) | n/a |
| send-saturation | 4096 | `zlink-cpp` | 1 | n/a (1 run) | n/a |

## Contaminated cells (FB-008, excluded from tables and judgement)

None. Every cell drained within the bound.

## Judgement (spec 7.2, FB-005, FB-011)

| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-cpp / zlink-c` | 1024 | n/a | **unsupported** | — | numerator zlink-cpp-request-window@1024 has 1 run(s); G5 needs 3; denominator zlink-c-request-window@1024 was not measured |
| `zlink-cpp / zlink-c` | 4096 | n/a | **unsupported** | — | numerator zlink-cpp-request-window@4096 has 1 run(s); G5 needs 3; denominator zlink-c-request-window@4096 was not measured |
| `zlink-framework-cpp / zlink-cpp` | 1024 | n/a | **unsupported** | — | numerator zlink-framework-cpp-request-window@1024 was not measured; denominator zlink-cpp-request-window@1024 has 1 run(s); G5 needs 3 |
| `zlink-framework-cpp / zlink-cpp` | 4096 | n/a | **unsupported** | — | numerator zlink-framework-cpp-request-window@4096 was not measured; denominator zlink-cpp-request-window@4096 has 1 run(s); G5 needs 3 |

**cpp: incomplete** — 4 of 4 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- cpp-dealer-1: read structured with-grpc-cell-v1
