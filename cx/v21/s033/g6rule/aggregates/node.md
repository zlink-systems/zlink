## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-node               | 1024B    |        3.20 KOPS |    3.27 MB/s |     0.310 ms |     0.449 ms |     0.701 ms |       6.3% |   241.3 MB |       3.1% |    92.3 MB |
      | zlink-c                 | 1024B    |       15.17 KOPS |   15.54 MB/s |     0.060 ms |     0.072 ms |     0.086 ms |       4.4% |     9.8 MB |       3.3% |     8.1 MB |
      | zlink-framework-node    | 1024B    |        5.43 KOPS |    5.56 MB/s |     0.181 ms |     0.242 ms |     0.375 ms |       5.3% |   145.0 MB |       3.9% |   138.6 MB |
      | zlink-node              | 1024B    |        9.08 KOPS |    9.30 MB/s |     0.108 ms |     0.140 ms |     0.264 ms |      4.6%* |   143.6 MB |       7.9% |    77.0 MB |
      | grpc-node               | 4096B    |        3.04 KOPS |   12.46 MB/s |     0.324 ms |     0.470 ms |     0.749 ms |       6.5% |   230.2 MB |       3.2% |    98.2 MB |
      | zlink-c                 | 4096B    |       14.33 KOPS |   58.68 MB/s |     0.063 ms |     0.077 ms |     0.096 ms |       4.3% |     9.6 MB |       3.3% |     8.3 MB |
      | zlink-framework-node    | 4096B    |        5.08 KOPS |   20.81 MB/s |     0.193 ms |     0.253 ms |     0.395 ms |       5.3% |   157.4 MB |       3.8% |   149.5 MB |
      | zlink-node              | 4096B    |        8.81 KOPS |   36.10 MB/s |     0.111 ms |     0.145 ms |     0.286 ms |      4.5%* |   157.9 MB |       8.1% |    76.9 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      127.49 KOPS |  130.55 MB/s |   109.022 ms |   118.773 ms |   120.315 ms |      32.3% |    74.8 MB |       8.8% |    11.0 MB |
      | zlink-framework-node    | 1024B    |       16.49 KOPS |   16.88 MB/s |   549.749 ms |   586.234 ms |   589.729 ms |     12.9%* |   517.6 MB |       6.2% |   146.7 MB |
      | zlink-node              | 1024B    |       31.68 KOPS |   32.44 MB/s |     1.248 ms |     4.870 ms |    18.864 ms |     11.7%* |   153.8 MB |      12.4% |   100.1 MB |
      | zlink-c                 | 4096B    |       40.68 KOPS |  166.62 MB/s |     6.031 ms |     6.619 ms |     7.108 ms |      18.2% |    18.6 MB |       6.3% |     9.7 MB |
      | zlink-framework-node    | 4096B    |       19.17 KOPS |   78.54 MB/s |    17.580 ms |    21.116 ms |    29.053 ms |     10.9%* |   176.4 MB |       7.5% |   170.4 MB |
      | zlink-node              | 4096B    |       27.10 KOPS |  111.01 MB/s |     0.983 ms |     4.949 ms |     8.314 ms |     12.4%* |   172.5 MB |      11.8% |   106.7 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-node               | 1024B    |      5.77 KMSG/s |    5.91 MB/s |     0.624 ms |     0.967 ms |     3.099 ms |       6.8% |   243.3 MB |       2.7% |   128.6 MB |
      | zlink-c                 | 1024B    |    527.26 KMSG/s |  539.91 MB/s |    12.843 ms |     3.640 ms |     3.903 ms |      13.9% |    11.2 MB |       5.2% |    22.2 MB |
      | zlink-framework-node    | 1024B    |     76.57 KMSG/s |   78.40 MB/s |   112.518 ms |   140.116 ms |   146.287 ms |       9.5% |   173.8 MB |       7.5% |   161.5 MB |
      | zlink-node              | 1024B    |    117.79 KMSG/s |  120.61 MB/s |    64.752 ms |    86.204 ms |    91.252 ms |     10.9%* |   172.2 MB |       7.7% |   141.6 MB |
      | grpc-node               | 4096B    |      5.46 KMSG/s |   22.37 MB/s |     0.692 ms |     1.221 ms |     3.379 ms |       6.9% |   238.3 MB |       2.7% |   125.7 MB |
      | zlink-c                 | 4096B    |    207.41 KMSG/s |  849.54 MB/s |     0.904 ms |     1.178 ms |     1.880 ms |       5.0% |    11.2 MB |       2.9% |    15.1 MB |
      | zlink-framework-node    | 4096B    |     54.99 KMSG/s |  225.22 MB/s |    46.477 ms |    59.666 ms |    66.090 ms |       9.6% |   186.5 MB |       8.4% |   190.8 MB |
      | zlink-node              | 4096B    |     87.57 KMSG/s |  358.70 MB/s |     2.463 ms |    11.375 ms |    18.502 ms |     12.8%* |   181.9 MB |      11.1% |   156.3 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-node-request-serial,local,1024,throughput,3195.600
RESULT,current,grpc-node-request-serial,local,1024,bandwidth,3.272
RESULT,current,grpc-node-request-serial,local,1024,latency,0.310
RESULT,current,grpc-node-request-serial,local,1024,latency_p95,0.449
RESULT,current,grpc-node-request-serial,local,1024,latency_p99,0.701
RESULT,current,grpc-node-request-serial,local,1024,client_cpu_percent,6.325
RESULT,current,grpc-node-request-serial,local,1024,client_memory_mb,241.270
RESULT,current,grpc-node-request-serial,local,1024,server_cpu_percent,3.134
RESULT,current,grpc-node-request-serial,local,1024,server_memory_mb,92.289
RESULT,current,zlink-c-request-serial,local,1024,throughput,15171.400
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,15.536
RESULT,current,zlink-c-request-serial,local,1024,latency,0.060
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.072
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.086
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,4.397
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,9.809
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,3.312
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,8.105
RESULT,current,zlink-framework-node-request-serial,local,1024,throughput,5425.400
RESULT,current,zlink-framework-node-request-serial,local,1024,bandwidth,5.556
RESULT,current,zlink-framework-node-request-serial,local,1024,latency,0.181
RESULT,current,zlink-framework-node-request-serial,local,1024,latency_p95,0.242
RESULT,current,zlink-framework-node-request-serial,local,1024,latency_p99,0.375
RESULT,current,zlink-framework-node-request-serial,local,1024,client_cpu_percent,5.252
RESULT,current,zlink-framework-node-request-serial,local,1024,client_memory_mb,145.016
RESULT,current,zlink-framework-node-request-serial,local,1024,server_cpu_percent,3.945
RESULT,current,zlink-framework-node-request-serial,local,1024,server_memory_mb,138.578
RESULT,current,zlink-node-request-serial,local,1024,throughput,9081.400
RESULT,current,zlink-node-request-serial,local,1024,bandwidth,9.299
RESULT,current,zlink-node-request-serial,local,1024,latency,0.108
RESULT,current,zlink-node-request-serial,local,1024,latency_p95,0.140
RESULT,current,zlink-node-request-serial,local,1024,latency_p99,0.264
RESULT,current,zlink-node-request-serial,local,1024,client_cpu_percent,4.552
RESULT,current,zlink-node-request-serial,local,1024,client_memory_mb,143.605
RESULT,current,zlink-node-request-serial,local,1024,server_cpu_percent,7.903
RESULT,current,zlink-node-request-serial,local,1024,server_memory_mb,77.012
RESULT,current,grpc-node-request-serial,local,4096,throughput,3041.400
RESULT,current,grpc-node-request-serial,local,4096,bandwidth,12.458
RESULT,current,grpc-node-request-serial,local,4096,latency,0.324
RESULT,current,grpc-node-request-serial,local,4096,latency_p95,0.470
RESULT,current,grpc-node-request-serial,local,4096,latency_p99,0.749
RESULT,current,grpc-node-request-serial,local,4096,client_cpu_percent,6.463
RESULT,current,grpc-node-request-serial,local,4096,client_memory_mb,230.203
RESULT,current,grpc-node-request-serial,local,4096,server_cpu_percent,3.179
RESULT,current,grpc-node-request-serial,local,4096,server_memory_mb,98.230
RESULT,current,zlink-c-request-serial,local,4096,throughput,14326.200
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,58.680
RESULT,current,zlink-c-request-serial,local,4096,latency,0.063
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.077
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.096
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,4.284
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,9.598
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,3.349
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.270
RESULT,current,zlink-framework-node-request-serial,local,4096,throughput,5080.000
RESULT,current,zlink-framework-node-request-serial,local,4096,bandwidth,20.808
RESULT,current,zlink-framework-node-request-serial,local,4096,latency,0.193
RESULT,current,zlink-framework-node-request-serial,local,4096,latency_p95,0.253
RESULT,current,zlink-framework-node-request-serial,local,4096,latency_p99,0.395
RESULT,current,zlink-framework-node-request-serial,local,4096,client_cpu_percent,5.257
RESULT,current,zlink-framework-node-request-serial,local,4096,client_memory_mb,157.395
RESULT,current,zlink-framework-node-request-serial,local,4096,server_cpu_percent,3.827
RESULT,current,zlink-framework-node-request-serial,local,4096,server_memory_mb,149.500
RESULT,current,zlink-node-request-serial,local,4096,throughput,8812.400
RESULT,current,zlink-node-request-serial,local,4096,bandwidth,36.096
RESULT,current,zlink-node-request-serial,local,4096,latency,0.111
RESULT,current,zlink-node-request-serial,local,4096,latency_p95,0.145
RESULT,current,zlink-node-request-serial,local,4096,latency_p99,0.286
RESULT,current,zlink-node-request-serial,local,4096,client_cpu_percent,4.530
RESULT,current,zlink-node-request-serial,local,4096,client_memory_mb,157.855
RESULT,current,zlink-node-request-serial,local,4096,server_cpu_percent,8.078
RESULT,current,zlink-node-request-serial,local,4096,server_memory_mb,76.934
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,127491.800
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,130.552
RESULT,current,zlink-c-request-backpressure,local,1024,latency,109.022
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,118.773
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,120.315
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,32.333
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,74.762
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.823
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,10.953
RESULT,current,zlink-framework-node-request-backpressure,local,1024,throughput,16486.400
RESULT,current,zlink-framework-node-request-backpressure,local,1024,bandwidth,16.882
RESULT,current,zlink-framework-node-request-backpressure,local,1024,latency,549.749
RESULT,current,zlink-framework-node-request-backpressure,local,1024,latency_p95,586.234
RESULT,current,zlink-framework-node-request-backpressure,local,1024,latency_p99,589.729
RESULT,current,zlink-framework-node-request-backpressure,local,1024,client_cpu_percent,12.924
RESULT,current,zlink-framework-node-request-backpressure,local,1024,client_memory_mb,517.559
RESULT,current,zlink-framework-node-request-backpressure,local,1024,server_cpu_percent,6.157
RESULT,current,zlink-framework-node-request-backpressure,local,1024,server_memory_mb,146.719
RESULT,current,zlink-node-request-backpressure,local,1024,throughput,31684.000
RESULT,current,zlink-node-request-backpressure,local,1024,bandwidth,32.444
RESULT,current,zlink-node-request-backpressure,local,1024,latency,1.248
RESULT,current,zlink-node-request-backpressure,local,1024,latency_p95,4.870
RESULT,current,zlink-node-request-backpressure,local,1024,latency_p99,18.864
RESULT,current,zlink-node-request-backpressure,local,1024,client_cpu_percent,11.733
RESULT,current,zlink-node-request-backpressure,local,1024,client_memory_mb,153.848
RESULT,current,zlink-node-request-backpressure,local,1024,server_cpu_percent,12.421
RESULT,current,zlink-node-request-backpressure,local,1024,server_memory_mb,100.090
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,40679.100
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,166.622
RESULT,current,zlink-c-request-backpressure,local,4096,latency,6.031
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,6.619
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,7.108
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,18.192
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,18.641
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,6.274
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,9.686
RESULT,current,zlink-framework-node-request-backpressure,local,4096,throughput,19174.400
RESULT,current,zlink-framework-node-request-backpressure,local,4096,bandwidth,78.538
RESULT,current,zlink-framework-node-request-backpressure,local,4096,latency,17.580
RESULT,current,zlink-framework-node-request-backpressure,local,4096,latency_p95,21.116
RESULT,current,zlink-framework-node-request-backpressure,local,4096,latency_p99,29.053
RESULT,current,zlink-framework-node-request-backpressure,local,4096,client_cpu_percent,10.931
RESULT,current,zlink-framework-node-request-backpressure,local,4096,client_memory_mb,176.391
RESULT,current,zlink-framework-node-request-backpressure,local,4096,server_cpu_percent,7.454
RESULT,current,zlink-framework-node-request-backpressure,local,4096,server_memory_mb,170.438
RESULT,current,zlink-node-request-backpressure,local,4096,throughput,27102.000
RESULT,current,zlink-node-request-backpressure,local,4096,bandwidth,111.010
RESULT,current,zlink-node-request-backpressure,local,4096,latency,0.983
RESULT,current,zlink-node-request-backpressure,local,4096,latency_p95,4.949
RESULT,current,zlink-node-request-backpressure,local,4096,latency_p99,8.314
RESULT,current,zlink-node-request-backpressure,local,4096,client_cpu_percent,12.362
RESULT,current,zlink-node-request-backpressure,local,4096,client_memory_mb,172.520
RESULT,current,zlink-node-request-backpressure,local,4096,server_cpu_percent,11.781
RESULT,current,zlink-node-request-backpressure,local,4096,server_memory_mb,106.746
RESULT,current,grpc-node-send-saturation,local,1024,throughput,5774.400
RESULT,current,grpc-node-send-saturation,local,1024,bandwidth,5.913
RESULT,current,grpc-node-send-saturation,local,1024,latency,0.624
RESULT,current,grpc-node-send-saturation,local,1024,latency_p95,0.967
RESULT,current,grpc-node-send-saturation,local,1024,latency_p99,3.099
RESULT,current,grpc-node-send-saturation,local,1024,client_cpu_percent,6.839
RESULT,current,grpc-node-send-saturation,local,1024,client_memory_mb,243.312
RESULT,current,grpc-node-send-saturation,local,1024,server_cpu_percent,2.656
RESULT,current,grpc-node-send-saturation,local,1024,server_memory_mb,128.590
RESULT,current,zlink-c-send-saturation,local,1024,throughput,527255.600
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,539.910
RESULT,current,zlink-c-send-saturation,local,1024,latency,12.843
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,3.640
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,3.903
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,13.936
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,11.184
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,5.249
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,22.164
RESULT,current,zlink-framework-node-send-saturation,local,1024,throughput,76566.000
RESULT,current,zlink-framework-node-send-saturation,local,1024,bandwidth,78.404
RESULT,current,zlink-framework-node-send-saturation,local,1024,latency,112.518
RESULT,current,zlink-framework-node-send-saturation,local,1024,latency_p95,140.116
RESULT,current,zlink-framework-node-send-saturation,local,1024,latency_p99,146.287
RESULT,current,zlink-framework-node-send-saturation,local,1024,client_cpu_percent,9.519
RESULT,current,zlink-framework-node-send-saturation,local,1024,client_memory_mb,173.812
RESULT,current,zlink-framework-node-send-saturation,local,1024,server_cpu_percent,7.460
RESULT,current,zlink-framework-node-send-saturation,local,1024,server_memory_mb,161.523
RESULT,current,zlink-node-send-saturation,local,1024,throughput,117787.000
RESULT,current,zlink-node-send-saturation,local,1024,bandwidth,120.614
RESULT,current,zlink-node-send-saturation,local,1024,latency,64.752
RESULT,current,zlink-node-send-saturation,local,1024,latency_p95,86.204
RESULT,current,zlink-node-send-saturation,local,1024,latency_p99,91.252
RESULT,current,zlink-node-send-saturation,local,1024,client_cpu_percent,10.882
RESULT,current,zlink-node-send-saturation,local,1024,client_memory_mb,172.160
RESULT,current,zlink-node-send-saturation,local,1024,server_cpu_percent,7.711
RESULT,current,zlink-node-send-saturation,local,1024,server_memory_mb,141.645
RESULT,current,grpc-node-send-saturation,local,4096,throughput,5460.800
RESULT,current,grpc-node-send-saturation,local,4096,bandwidth,22.367
RESULT,current,grpc-node-send-saturation,local,4096,latency,0.692
RESULT,current,grpc-node-send-saturation,local,4096,latency_p95,1.221
RESULT,current,grpc-node-send-saturation,local,4096,latency_p99,3.379
RESULT,current,grpc-node-send-saturation,local,4096,client_cpu_percent,6.949
RESULT,current,grpc-node-send-saturation,local,4096,client_memory_mb,238.320
RESULT,current,grpc-node-send-saturation,local,4096,server_cpu_percent,2.677
RESULT,current,grpc-node-send-saturation,local,4096,server_memory_mb,125.715
RESULT,current,zlink-c-send-saturation,local,4096,throughput,207407.800
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,849.542
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.904
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,1.178
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,1.880
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,4.980
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,11.164
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,2.870
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,15.086
RESULT,current,zlink-framework-node-send-saturation,local,4096,throughput,54986.400
RESULT,current,zlink-framework-node-send-saturation,local,4096,bandwidth,225.224
RESULT,current,zlink-framework-node-send-saturation,local,4096,latency,46.477
RESULT,current,zlink-framework-node-send-saturation,local,4096,latency_p95,59.666
RESULT,current,zlink-framework-node-send-saturation,local,4096,latency_p99,66.090
RESULT,current,zlink-framework-node-send-saturation,local,4096,client_cpu_percent,9.637
RESULT,current,zlink-framework-node-send-saturation,local,4096,client_memory_mb,186.461
RESULT,current,zlink-framework-node-send-saturation,local,4096,server_cpu_percent,8.389
RESULT,current,zlink-framework-node-send-saturation,local,4096,server_memory_mb,190.762
RESULT,current,zlink-node-send-saturation,local,4096,throughput,87574.400
RESULT,current,zlink-node-send-saturation,local,4096,bandwidth,358.705
RESULT,current,zlink-node-send-saturation,local,4096,latency,2.463
RESULT,current,zlink-node-send-saturation,local,4096,latency_p95,11.375
RESULT,current,zlink-node-send-saturation,local,4096,latency_p99,18.502
RESULT,current,zlink-node-send-saturation,local,4096,client_cpu_percent,12.842
RESULT,current,zlink-node-send-saturation,local,4096,client_memory_mb,181.906
RESULT,current,zlink-node-send-saturation,local,4096,server_cpu_percent,11.065
RESULT,current,zlink-node-send-saturation,local,4096,server_memory_mb,156.297


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 3.196 | 0.310 | 0.449 | 0.701 | 6.3 | 1.01 | 241.3 | 3.1 | 92.3 | 331 |
| request-serial | 1024 | `zlink-c` | 15.171 | 0.060 | 0.072 | 0.086 | 4.4 | n/a | 9.8 | 3.3 | 8.1 | 0 |
| request-serial | 1024 | `zlink-framework-node` | 5.425 | 0.181 | 0.242 | 0.375 | 5.3 | 0.84 | 145.0 | 3.9 | 138.6 | 342 |
| request-serial | 1024 | `zlink-node` | 9.081 | 0.108 | 0.140 | 0.264 | 4.6* | 0.73 | 143.6 | 7.9 | 77.0 | 347 |
| request-serial | 4096 | `grpc-node` | 3.041 | 0.324 | 0.470 | 0.749 | 6.5 | 1.03 | 230.2 | 3.2 | 98.2 | 330 |
| request-serial | 4096 | `zlink-c` | 14.326 | 0.063 | 0.077 | 0.096 | 4.3 | n/a | 9.6 | 3.3 | 8.3 | 0 |
| request-serial | 4096 | `zlink-framework-node` | 5.080 | 0.193 | 0.253 | 0.395 | 5.3 | 0.84 | 157.4 | 3.8 | 149.5 | 340 |
| request-serial | 4096 | `zlink-node` | 8.812 | 0.111 | 0.145 | 0.286 | 4.5* | 0.72 | 157.9 | 8.1 | 76.9 | 352 |
| request-backpressure | 1024 | `zlink-c` | 127.492 | 109.022 | 118.773 | 120.315 | 32.3 | n/a | 74.8 | 8.8 | 11.0 | 21 |
| request-backpressure | 1024 | `zlink-framework-node` | 16.486 | 549.749 | 586.234 | 589.729 | 12.9* | 2.07 | 517.6 | 6.2 | 146.7 | 378 |
| request-backpressure | 1024 | `zlink-node` | 31.684 | 1.248 | 4.870 | 18.864 | 11.7* | 1.88 | 153.8 | 12.4 | 100.1 | 412 |
| request-backpressure | 4096 | `zlink-c` | 40.679 | 6.031 | 6.619 | 7.108 | 18.2 | n/a | 18.6 | 6.3 | 9.7 | 1 |
| request-backpressure | 4096 | `zlink-framework-node` | 19.174 | 17.580 | 21.116 | 29.053 | 10.9* | 1.75 | 176.4 | 7.5 | 170.4 | 381 |
| request-backpressure | 4096 | `zlink-node` | 27.102 | 0.983 | 4.949 | 8.314 | 12.4* | 1.98 | 172.5 | 11.8 | 106.7 | 405 |
| send-saturation | 1024 | `grpc-node` | 5.774 | 0.624 | 0.967 | 3.099 | 6.8 | 1.09 | 243.3 | 2.7 | 128.6 | 337 |
| send-saturation | 1024 | `zlink-c` | 527.256 | 12.843 | 3.640 | 3.903 | 13.9 | n/a | 11.2 | 5.2 | 22.2 | 2 |
| send-saturation | 1024 | `zlink-framework-node` | 76.566 | 112.518 | 140.116 | 146.287 | 9.5 | 1.52 | 173.8 | 7.5 | 161.5 | 496 |
| send-saturation | 1024 | `zlink-node` | 117.787 | 64.752 | 86.204 | 91.252 | 10.9* | 1.74 | 172.2 | 7.7 | 141.6 | 557 |
| send-saturation | 4096 | `grpc-node` | 5.461 | 0.692 | 1.221 | 3.379 | 6.9 | 1.11 | 238.3 | 2.7 | 125.7 | 334 |
| send-saturation | 4096 | `zlink-c` | 207.408 | 0.904 | 1.178 | 1.880 | 5.0 | n/a | 11.2 | 2.9 | 15.1 | 1 |
| send-saturation | 4096 | `zlink-framework-node` | 54.986 | 46.477 | 59.666 | 66.090 | 9.6 | 1.54 | 186.5 | 8.4 | 190.8 | 445 |
| send-saturation | 4096 | `zlink-node` | 87.574 | 2.463 | 11.375 | 18.502 | 12.8* | 2.05 | 181.9 | 11.1 | 156.3 | 581 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | execution-model ceiling | G6 status | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 1 | — | 0 | 1.0 | 331 | no | 1.01 | event_loop_utilization | 0.731 | 1 | 1 | no | 15979 | 0 | — |
| request-serial | 1024 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | 1 | not judged | n/a | 0 | — |
| request-serial | 1024 | `zlink-framework-node` | 1 | — | 0 | 1.0 | 342 | no | 0.84 | event_loop_utilization | 0.502 | 1 | 1 | no | 27128 | 0 | — |
| request-serial | 1024 | `zlink-node` | 1 | — | 0 | 1.0 | 347 | no | 0.73 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 45408 | 0 | — |
| request-serial | 4096 | `grpc-node` | 1 | — | 0 | 1.0 | 330 | no | 1.03 | event_loop_utilization | 0.733 | 1 | 1 | no | 15208 | 0 | — |
| request-serial | 4096 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | 1 | not judged | n/a | 0 | — |
| request-serial | 4096 | `zlink-framework-node` | 1 | — | 0 | 1.0 | 340 | no | 0.84 | event_loop_utilization | 0.481 | 1 | 1 | no | 25401 | 0 | — |
| request-serial | 4096 | `zlink-node` | 1 | — | 0 | 1.0 | 352 | no | 0.72 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 44063 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 15783 | — | 0 | 13899.5 | 21 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 14031 | — |
| request-backpressure | 1024 | `zlink-framework-node` | 10131 | — | 0 | 9063.4 | 378 | no | 2.07 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 92555 | 0 | — |
| request-backpressure | 1024 | `zlink-node` | 1211 (low 957) | — | 0 | 39.5 | 412 | no | 1.88 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 158424 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 922 (low 770) | — | 0 | 245.3 | 1 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 205 | — |
| request-backpressure | 4096 | `zlink-framework-node` | 380 | — | 0 | 337.1 | 381 | no | 1.75 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 96150 | 0 | — |
| request-backpressure | 4096 | `zlink-node` | 408 (low 304) | — | 0 | 26.7 | 405 | no | 1.98 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 135512 | 0 | — |
| send-saturation | 1024 | `grpc-node` | 8 | — | 0 | 3.6 | 337 | no | 1.09 | event_loop_utilization | 0.763 | 1 | 1 | no | 28872 | 0 | server |
| send-saturation | 1024 | `zlink-c` | 8 | — | 0 | 6771.5 | 2 | no | n/a | client_cores | n/a | — | 8 | not judged | 2636278 | 0 | server |
| send-saturation | 1024 | `zlink-framework-node` | 8 | — | 0 | 8615.1 | 496 | no | 1.52 | event_loop_utilization | 0.940 | 1 | 1 | no | 382830 | 0 | server |
| send-saturation | 1024 | `zlink-node` | 8 | — | 0 | 7626.9 | 557 | no | 1.74 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 588935 | 0 | server |
| send-saturation | 4096 | `grpc-node` | 8 | — | 0 | 3.8 | 334 | no | 1.11 | event_loop_utilization | 0.767 | 1 | 1 | no | 27304 | 0 | server |
| send-saturation | 4096 | `zlink-c` | 8 | — | 0 | 187.6 | 1 | no | n/a | client_cores | n/a | — | 8 | not judged | 1037039 | 0 | server |
| send-saturation | 4096 | `zlink-framework-node` | 8 | — | 0 | 2555.6 | 445 | no | 1.54 | event_loop_utilization | 0.919 | 1 | 1 | no | 274932 | 0 | server |
| send-saturation | 4096 | `zlink-node` | 8 | — | 0 | 215.7 | 581 | no | 2.05 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 437872 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 1 | 1 | http://127.0.0.1:5220/bench/start |
| request-serial | 1024 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 1024 | `zlink-framework-node` | 1 | 1 | http://127.0.0.1:5232/bench/start |
| request-serial | 1024 | `zlink-node` | 1 | 1 | http://127.0.0.1:5225/bench/start |
| request-serial | 4096 | `grpc-node` | 1 | 1 | http://127.0.0.1:5220/bench/start |
| request-serial | 4096 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 4096 | `zlink-framework-node` | 1 | 1 | http://127.0.0.1:5232/bench/start |
| request-serial | 4096 | `zlink-node` | 1 | 1 | http://127.0.0.1:5225/bench/start |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-framework-node` | 8 | unbounded | http://127.0.0.1:5232/bench/start |
| request-backpressure | 1024 | `zlink-node` | 8 | unbounded | http://127.0.0.1:5225/bench/start |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-framework-node` | 8 | unbounded | http://127.0.0.1:5232/bench/start |
| request-backpressure | 4096 | `zlink-node` | 8 | unbounded | http://127.0.0.1:5225/bench/start |
| send-saturation | 1024 | `grpc-node` | 8 | 1 | http://127.0.0.1:5220/bench/start |
| send-saturation | 1024 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 1024 | `zlink-framework-node` | 8 | 1 | http://127.0.0.1:5232/bench/start |
| send-saturation | 1024 | `zlink-node` | 8 | 1 | http://127.0.0.1:5225/bench/start |
| send-saturation | 4096 | `grpc-node` | 8 | 1 | http://127.0.0.1:5220/bench/start |
| send-saturation | 4096 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 4096 | `zlink-framework-node` | 8 | 1 | http://127.0.0.1:5232/bench/start |
| send-saturation | 4096 | `zlink-node` | 8 | 1 | http://127.0.0.1:5225/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 3 | 1.0% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 7.9% | pass |
| request-serial | 1024 | `zlink-framework-node` | 3 | 4.4% | pass |
| request-serial | 1024 | `zlink-node` | 3 | 3.7% | pass |
| request-serial | 4096 | `grpc-node` | 3 | 0.9% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 2.9% | pass |
| request-serial | 4096 | `zlink-framework-node` | 3 | 4.6% | pass |
| request-serial | 4096 | `zlink-node` | 3 | 6.9% | pass |
| request-backpressure | 1024 | `zlink-c` | 1 | n/a (1 run) | n/a |
| request-backpressure | 1024 | `zlink-framework-node` | 3 | 3.1% | pass |
| request-backpressure | 1024 | `zlink-node` | 3 | 4.0% | pass |
| request-backpressure | 4096 | `zlink-c` | 2 | 66.4% | n/a |
| request-backpressure | 4096 | `zlink-framework-node` | 3 | 11.9% | **fail** |
| request-backpressure | 4096 | `zlink-node` | 3 | 12.2% | **fail** |
| send-saturation | 1024 | `grpc-node` | 3 | 2.4% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 71.1% | **fail** |
| send-saturation | 1024 | `zlink-framework-node` | 3 | 2.1% | pass |
| send-saturation | 1024 | `zlink-node` | 3 | 3.3% | pass |
| send-saturation | 4096 | `grpc-node` | 3 | 0.3% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 64.0% | **fail** |
| send-saturation | 4096 | `zlink-framework-node` | 3 | 6.7% | pass |
| send-saturation | 4096 | `zlink-node` | 3 | 7.0% | pass |

## Excluded cells (FB-008, excluded from tables and judgement)

| Run | Cell | Reason |
|---|---|---|
| run2 | `zlink-c-request-backpressure@1024` |  |
| run3 | `zlink-c-request-backpressure@1024` |  |
| run3 | `zlink-c-request-backpressure@4096` |  |

## Incomplete server-driven cells (excluded from tables and judgement)

None. Every server-driven source has target stats.

## Judgement (spec 7.2)

| Formula | Pattern | Payload | Value | Status | Verdict | Reason |
|---|---:|---:|---:|---|---|---|
| `zlink-node / zlink-c` | request-backpressure | 1024 | (0.249) | **unsupported** | — | denominator zlink-c-request-backpressure@1024 has 1 run(s); G5 needs 3 (threshold 0.80) |
| `zlink-node / zlink-c` | request-backpressure | 4096 | (0.666) | **unsupported** | — | numerator zlink-node-request-backpressure@4096 fails G5 at 12.2% (limit 10%); denominator zlink-c-request-backpressure@4096 has 2 run(s); G5 needs 3 (threshold 0.80) |
| `zlink-framework-node / zlink-node` | request-serial | 1024 | 0.597 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | request-serial | 4096 | 0.576 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | request-backpressure | 1024 | 0.520 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | request-backpressure | 4096 | (0.707) | **unsupported** | — | numerator zlink-framework-node-request-backpressure@4096 fails G5 at 11.9% (limit 10%); denominator zlink-node-request-backpressure@4096 fails G5 at 12.2% (limit 10%) (threshold 0.60) |
| `zlink-framework-node / zlink-node` | send-saturation | 1024 | 0.650 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | send-saturation | 4096 | 0.628 | published | pass | both rows pass G5 (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**node: incomplete** — 3 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 6 file(s)
- run2: read cell data from 6 file(s)
- run3: read cell data from 6 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)
