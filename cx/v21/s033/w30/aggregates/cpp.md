## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |        7.70 KOPS |    7.89 MB/s |     0.123 ms |     0.148 ms |     0.173 ms |       4.2% |    24.9 MB |       4.5% |    24.3 MB |
      | zlink-c                 | 1024B    |       15.17 KOPS |   15.54 MB/s |     0.060 ms |     0.072 ms |     0.086 ms |       4.4% |     9.8 MB |       3.3% |     8.1 MB |
      | zlink-cpp               | 1024B    |       13.74 KOPS |   14.07 MB/s |     0.067 ms |     0.080 ms |     0.097 ms |       4.1% |    21.3 MB |       3.1% |    17.8 MB |
      | zlink-framework-cpp     | 1024B    |        8.82 KOPS |    9.03 MB/s |     0.107 ms |     0.134 ms |     0.163 ms |       4.2% |    30.3 MB |       4.0% |    26.9 MB |
      | grpc-cpp                | 4096B    |        7.35 KOPS |   30.10 MB/s |     0.129 ms |     0.155 ms |     0.181 ms |       4.2% |    25.1 MB |       4.5% |    24.7 MB |
      | zlink-c                 | 4096B    |       14.33 KOPS |   58.68 MB/s |     0.063 ms |     0.077 ms |     0.096 ms |       4.3% |     9.6 MB |       3.3% |     8.3 MB |
      | zlink-cpp               | 4096B    |       12.87 KOPS |   52.73 MB/s |     0.072 ms |     0.086 ms |     0.107 ms |       4.0% |    21.5 MB |       3.1% |    17.9 MB |
      | zlink-framework-cpp     | 4096B    |        7.99 KOPS |   32.73 MB/s |     0.119 ms |     0.146 ms |     0.177 ms |       4.3% |    29.7 MB |       3.7% |    26.8 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      127.49 KOPS |  130.55 MB/s |   109.022 ms |   118.773 ms |   120.315 ms |      32.3% |    74.8 MB |       8.8% |    11.0 MB |
      | zlink-cpp               | 1024B    |      151.79 KOPS |  155.44 MB/s |    90.812 ms |    99.693 ms |   101.542 ms |      40.8% |    97.9 MB |      11.5% |    30.9 MB |
      | zlink-framework-cpp     | 1024B    |       47.54 KOPS |   48.68 MB/s |     5.385 ms |    15.876 ms |    17.623 ms |      29.0% |    38.6 MB |      20.2% |    36.4 MB |
      | zlink-c                 | 4096B    |       40.68 KOPS |  166.62 MB/s |     6.031 ms |     6.619 ms |     7.108 ms |      18.2% |    18.6 MB |       6.3% |     9.7 MB |
      | zlink-cpp               | 4096B    |      141.45 KOPS |  579.39 MB/s |     2.184 ms |     3.459 ms |     4.137 ms |      34.2% |    26.6 MB |      16.2% |    34.3 MB |
      | zlink-framework-cpp     | 4096B    |       45.81 KOPS |  187.62 MB/s |     3.224 ms |     8.498 ms |    10.887 ms |      29.8% |    37.6 MB |      20.0% |    34.7 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |     24.39 KMSG/s |   24.98 MB/s |     0.165 ms |     0.243 ms |     0.283 ms |      12.1% |    25.9 MB |      13.7% |    29.6 MB |
      | zlink-c                 | 1024B    |    527.26 KMSG/s |  539.91 MB/s |    12.843 ms |     3.640 ms |     3.903 ms |      13.9% |    11.2 MB |       5.2% |    22.2 MB |
      | zlink-cpp               | 1024B    |    592.11 KMSG/s |  606.32 MB/s |     1.225 ms |     1.473 ms |     2.417 ms |      30.5% |    23.6 MB |      10.6% |    55.3 MB |
      | zlink-framework-cpp     | 1024B    |    112.94 KMSG/s |  115.65 MB/s |     1.229 ms |     5.102 ms |     6.601 ms |      16.1% |    29.4 MB |      15.8% |    42.3 MB |
      | grpc-cpp                | 4096B    |     23.99 KMSG/s |   98.25 MB/s |     0.169 ms |     0.248 ms |     0.287 ms |      12.2% |    26.0 MB |      13.7% |    31.0 MB |
      | zlink-c                 | 4096B    |    207.41 KMSG/s |  849.54 MB/s |     0.904 ms |     1.178 ms |     1.880 ms |       5.0% |    11.2 MB |       2.9% |    15.1 MB |
      | zlink-cpp               | 4096B    |    255.99 KMSG/s | 1048.55 MB/s |     1.886 ms |     6.638 ms |     7.055 ms |      18.1% |    23.4 MB |      10.1% |    46.4 MB |
      | zlink-framework-cpp     | 4096B    |    110.44 KMSG/s |  452.34 MB/s |     1.392 ms |     6.242 ms |     9.135 ms |      16.5% |    29.6 MB |      16.0% |    48.2 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-cpp-request-serial,local,1024,throughput,7703.929
RESULT,current,grpc-cpp-request-serial,local,1024,bandwidth,7.889
RESULT,current,grpc-cpp-request-serial,local,1024,latency,0.123
RESULT,current,grpc-cpp-request-serial,local,1024,latency_p95,0.148
RESULT,current,grpc-cpp-request-serial,local,1024,latency_p99,0.173
RESULT,current,grpc-cpp-request-serial,local,1024,client_cpu_percent,4.165
RESULT,current,grpc-cpp-request-serial,local,1024,client_memory_mb,24.945
RESULT,current,grpc-cpp-request-serial,local,1024,server_cpu_percent,4.548
RESULT,current,grpc-cpp-request-serial,local,1024,server_memory_mb,24.273
RESULT,current,zlink-c-request-serial,local,1024,throughput,15171.400
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,15.536
RESULT,current,zlink-c-request-serial,local,1024,latency,0.060
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.072
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.086
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,4.397
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,9.809
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,3.312
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,8.105
RESULT,current,zlink-cpp-request-serial,local,1024,throughput,13743.026
RESULT,current,zlink-cpp-request-serial,local,1024,bandwidth,14.073
RESULT,current,zlink-cpp-request-serial,local,1024,latency,0.067
RESULT,current,zlink-cpp-request-serial,local,1024,latency_p95,0.080
RESULT,current,zlink-cpp-request-serial,local,1024,latency_p99,0.097
RESULT,current,zlink-cpp-request-serial,local,1024,client_cpu_percent,4.066
RESULT,current,zlink-cpp-request-serial,local,1024,client_memory_mb,21.273
RESULT,current,zlink-cpp-request-serial,local,1024,server_cpu_percent,3.101
RESULT,current,zlink-cpp-request-serial,local,1024,server_memory_mb,17.758
RESULT,current,zlink-framework-cpp-request-serial,local,1024,throughput,8822.297
RESULT,current,zlink-framework-cpp-request-serial,local,1024,bandwidth,9.034
RESULT,current,zlink-framework-cpp-request-serial,local,1024,latency,0.107
RESULT,current,zlink-framework-cpp-request-serial,local,1024,latency_p95,0.134
RESULT,current,zlink-framework-cpp-request-serial,local,1024,latency_p99,0.163
RESULT,current,zlink-framework-cpp-request-serial,local,1024,client_cpu_percent,4.229
RESULT,current,zlink-framework-cpp-request-serial,local,1024,client_memory_mb,30.270
RESULT,current,zlink-framework-cpp-request-serial,local,1024,server_cpu_percent,4.018
RESULT,current,zlink-framework-cpp-request-serial,local,1024,server_memory_mb,26.859
RESULT,current,grpc-cpp-request-serial,local,4096,throughput,7347.712
RESULT,current,grpc-cpp-request-serial,local,4096,bandwidth,30.096
RESULT,current,grpc-cpp-request-serial,local,4096,latency,0.129
RESULT,current,grpc-cpp-request-serial,local,4096,latency_p95,0.155
RESULT,current,grpc-cpp-request-serial,local,4096,latency_p99,0.181
RESULT,current,grpc-cpp-request-serial,local,4096,client_cpu_percent,4.173
RESULT,current,grpc-cpp-request-serial,local,4096,client_memory_mb,25.051
RESULT,current,grpc-cpp-request-serial,local,4096,server_cpu_percent,4.476
RESULT,current,grpc-cpp-request-serial,local,4096,server_memory_mb,24.676
RESULT,current,zlink-c-request-serial,local,4096,throughput,14326.200
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,58.680
RESULT,current,zlink-c-request-serial,local,4096,latency,0.063
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.077
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.096
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,4.284
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,9.598
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,3.349
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.270
RESULT,current,zlink-cpp-request-serial,local,4096,throughput,12872.648
RESULT,current,zlink-cpp-request-serial,local,4096,bandwidth,52.726
RESULT,current,zlink-cpp-request-serial,local,4096,latency,0.072
RESULT,current,zlink-cpp-request-serial,local,4096,latency_p95,0.086
RESULT,current,zlink-cpp-request-serial,local,4096,latency_p99,0.107
RESULT,current,zlink-cpp-request-serial,local,4096,client_cpu_percent,4.014
RESULT,current,zlink-cpp-request-serial,local,4096,client_memory_mb,21.516
RESULT,current,zlink-cpp-request-serial,local,4096,server_cpu_percent,3.078
RESULT,current,zlink-cpp-request-serial,local,4096,server_memory_mb,17.926
RESULT,current,zlink-framework-cpp-request-serial,local,4096,throughput,7991.508
RESULT,current,zlink-framework-cpp-request-serial,local,4096,bandwidth,32.733
RESULT,current,zlink-framework-cpp-request-serial,local,4096,latency,0.119
RESULT,current,zlink-framework-cpp-request-serial,local,4096,latency_p95,0.146
RESULT,current,zlink-framework-cpp-request-serial,local,4096,latency_p99,0.177
RESULT,current,zlink-framework-cpp-request-serial,local,4096,client_cpu_percent,4.268
RESULT,current,zlink-framework-cpp-request-serial,local,4096,client_memory_mb,29.660
RESULT,current,zlink-framework-cpp-request-serial,local,4096,server_cpu_percent,3.709
RESULT,current,zlink-framework-cpp-request-serial,local,4096,server_memory_mb,26.828
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,127491.800
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,130.552
RESULT,current,zlink-c-request-backpressure,local,1024,latency,109.022
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,118.773
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,120.315
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,32.333
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,74.762
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.823
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,10.953
RESULT,current,zlink-cpp-request-backpressure,local,1024,throughput,151792.749
RESULT,current,zlink-cpp-request-backpressure,local,1024,bandwidth,155.436
RESULT,current,zlink-cpp-request-backpressure,local,1024,latency,90.812
RESULT,current,zlink-cpp-request-backpressure,local,1024,latency_p95,99.693
RESULT,current,zlink-cpp-request-backpressure,local,1024,latency_p99,101.542
RESULT,current,zlink-cpp-request-backpressure,local,1024,client_cpu_percent,40.780
RESULT,current,zlink-cpp-request-backpressure,local,1024,client_memory_mb,97.867
RESULT,current,zlink-cpp-request-backpressure,local,1024,server_cpu_percent,11.464
RESULT,current,zlink-cpp-request-backpressure,local,1024,server_memory_mb,30.902
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,throughput,47540.232
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,bandwidth,48.681
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,latency,5.385
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,latency_p95,15.876
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,latency_p99,17.623
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,client_cpu_percent,28.966
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,client_memory_mb,38.582
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,server_cpu_percent,20.166
RESULT,current,zlink-framework-cpp-request-backpressure,local,1024,server_memory_mb,36.438
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,40679.100
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,166.622
RESULT,current,zlink-c-request-backpressure,local,4096,latency,6.031
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,6.619
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,7.108
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,18.192
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,18.641
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,6.274
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,9.686
RESULT,current,zlink-cpp-request-backpressure,local,4096,throughput,141453.739
RESULT,current,zlink-cpp-request-backpressure,local,4096,bandwidth,579.395
RESULT,current,zlink-cpp-request-backpressure,local,4096,latency,2.184
RESULT,current,zlink-cpp-request-backpressure,local,4096,latency_p95,3.459
RESULT,current,zlink-cpp-request-backpressure,local,4096,latency_p99,4.137
RESULT,current,zlink-cpp-request-backpressure,local,4096,client_cpu_percent,34.233
RESULT,current,zlink-cpp-request-backpressure,local,4096,client_memory_mb,26.578
RESULT,current,zlink-cpp-request-backpressure,local,4096,server_cpu_percent,16.201
RESULT,current,zlink-cpp-request-backpressure,local,4096,server_memory_mb,34.336
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,throughput,45806.339
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,bandwidth,187.623
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,latency,3.224
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,latency_p95,8.498
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,latency_p99,10.887
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,client_cpu_percent,29.752
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,client_memory_mb,37.570
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,server_cpu_percent,20.042
RESULT,current,zlink-framework-cpp-request-backpressure,local,4096,server_memory_mb,34.703
RESULT,current,grpc-cpp-send-saturation,local,1024,throughput,24392.569
RESULT,current,grpc-cpp-send-saturation,local,1024,bandwidth,24.978
RESULT,current,grpc-cpp-send-saturation,local,1024,latency,0.165
RESULT,current,grpc-cpp-send-saturation,local,1024,latency_p95,0.243
RESULT,current,grpc-cpp-send-saturation,local,1024,latency_p99,0.283
RESULT,current,grpc-cpp-send-saturation,local,1024,client_cpu_percent,12.120
RESULT,current,grpc-cpp-send-saturation,local,1024,client_memory_mb,25.914
RESULT,current,grpc-cpp-send-saturation,local,1024,server_cpu_percent,13.732
RESULT,current,grpc-cpp-send-saturation,local,1024,server_memory_mb,29.582
RESULT,current,zlink-c-send-saturation,local,1024,throughput,527255.600
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,539.910
RESULT,current,zlink-c-send-saturation,local,1024,latency,12.843
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,3.640
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,3.903
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,13.936
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,11.184
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,5.249
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,22.164
RESULT,current,zlink-cpp-send-saturation,local,1024,throughput,592105.491
RESULT,current,zlink-cpp-send-saturation,local,1024,bandwidth,606.316
RESULT,current,zlink-cpp-send-saturation,local,1024,latency,1.225
RESULT,current,zlink-cpp-send-saturation,local,1024,latency_p95,1.473
RESULT,current,zlink-cpp-send-saturation,local,1024,latency_p99,2.417
RESULT,current,zlink-cpp-send-saturation,local,1024,client_cpu_percent,30.511
RESULT,current,zlink-cpp-send-saturation,local,1024,client_memory_mb,23.633
RESULT,current,zlink-cpp-send-saturation,local,1024,server_cpu_percent,10.592
RESULT,current,zlink-cpp-send-saturation,local,1024,server_memory_mb,55.285
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,throughput,112938.666
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,bandwidth,115.649
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,latency,1.229
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,latency_p95,5.102
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,latency_p99,6.601
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,client_cpu_percent,16.084
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,client_memory_mb,29.363
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,server_cpu_percent,15.821
RESULT,current,zlink-framework-cpp-send-saturation,local,1024,server_memory_mb,42.328
RESULT,current,grpc-cpp-send-saturation,local,4096,throughput,23986.772
RESULT,current,grpc-cpp-send-saturation,local,4096,bandwidth,98.250
RESULT,current,grpc-cpp-send-saturation,local,4096,latency,0.169
RESULT,current,grpc-cpp-send-saturation,local,4096,latency_p95,0.248
RESULT,current,grpc-cpp-send-saturation,local,4096,latency_p99,0.287
RESULT,current,grpc-cpp-send-saturation,local,4096,client_cpu_percent,12.163
RESULT,current,grpc-cpp-send-saturation,local,4096,client_memory_mb,25.980
RESULT,current,grpc-cpp-send-saturation,local,4096,server_cpu_percent,13.721
RESULT,current,grpc-cpp-send-saturation,local,4096,server_memory_mb,31.000
RESULT,current,zlink-c-send-saturation,local,4096,throughput,207407.800
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,849.542
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.904
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,1.178
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,1.880
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,4.980
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,11.164
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,2.870
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,15.086
RESULT,current,zlink-cpp-send-saturation,local,4096,throughput,255994.430
RESULT,current,zlink-cpp-send-saturation,local,4096,bandwidth,1048.553
RESULT,current,zlink-cpp-send-saturation,local,4096,latency,1.886
RESULT,current,zlink-cpp-send-saturation,local,4096,latency_p95,6.638
RESULT,current,zlink-cpp-send-saturation,local,4096,latency_p99,7.055
RESULT,current,zlink-cpp-send-saturation,local,4096,client_cpu_percent,18.078
RESULT,current,zlink-cpp-send-saturation,local,4096,client_memory_mb,23.449
RESULT,current,zlink-cpp-send-saturation,local,4096,server_cpu_percent,10.128
RESULT,current,zlink-cpp-send-saturation,local,4096,server_memory_mb,46.383
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,throughput,110435.178
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,bandwidth,452.342
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,latency,1.392
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,latency_p95,6.242
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,latency_p99,9.135
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,client_cpu_percent,16.486
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,client_memory_mb,29.629
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,server_cpu_percent,15.968
RESULT,current,zlink-framework-cpp-send-saturation,local,4096,server_memory_mb,48.164


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 7.704 | 0.123 | 0.148 | 0.173 | 4.2 | 0.67 | 24.9 | 4.5 | 24.3 | 330 |
| request-serial | 1024 | `zlink-c` | 15.171 | 0.060 | 0.072 | 0.086 | 4.4 | n/a | 9.8 | 3.3 | 8.1 | 0 |
| request-serial | 1024 | `zlink-cpp` | 13.743 | 0.067 | 0.080 | 0.097 | 4.1 | 0.65 | 21.3 | 3.1 | 17.8 | 339 |
| request-serial | 1024 | `zlink-framework-cpp` | 8.822 | 0.107 | 0.134 | 0.163 | 4.2 | 0.68 | 30.3 | 4.0 | 26.9 | 336 |
| request-serial | 4096 | `grpc-cpp` | 7.348 | 0.129 | 0.155 | 0.181 | 4.2 | 0.67 | 25.1 | 4.5 | 24.7 | 332 |
| request-serial | 4096 | `zlink-c` | 14.326 | 0.063 | 0.077 | 0.096 | 4.3 | n/a | 9.6 | 3.3 | 8.3 | 0 |
| request-serial | 4096 | `zlink-cpp` | 12.873 | 0.072 | 0.086 | 0.107 | 4.0 | 0.64 | 21.5 | 3.1 | 17.9 | 344 |
| request-serial | 4096 | `zlink-framework-cpp` | 7.992 | 0.119 | 0.146 | 0.177 | 4.3 | 0.68 | 29.7 | 3.7 | 26.8 | 334 |
| request-backpressure | 1024 | `zlink-c` | 127.492 | 109.022 | 118.773 | 120.315 | 32.3 | n/a | 74.8 | 8.8 | 11.0 | 21 |
| request-backpressure | 1024 | `zlink-cpp` | 151.793 | 90.812 | 99.693 | 101.542 | 40.8 | 6.52 | 97.9 | 11.5 | 30.9 | 538 |
| request-backpressure | 1024 | `zlink-framework-cpp` | 47.540 | 5.385 | 15.876 | 17.623 | 29.0 | 4.63 | 38.6 | 20.2 | 36.4 | 397 |
| request-backpressure | 4096 | `zlink-c` | 40.679 | 6.031 | 6.619 | 7.108 | 18.2 | n/a | 18.6 | 6.3 | 9.7 | 1 |
| request-backpressure | 4096 | `zlink-cpp` | 141.454 | 2.184 | 3.459 | 4.137 | 34.2 | 5.48 | 26.6 | 16.2 | 34.3 | 529 |
| request-backpressure | 4096 | `zlink-framework-cpp` | 45.806 | 3.224 | 8.498 | 10.887 | 29.8 | 4.76 | 37.6 | 20.0 | 34.7 | 394 |
| send-saturation | 1024 | `grpc-cpp` | 24.393 | 0.165 | 0.243 | 0.283 | 12.1 | 1.94 | 25.9 | 13.7 | 29.6 | 358 |
| send-saturation | 1024 | `zlink-c` | 527.256 | 12.843 | 3.640 | 3.903 | 13.9 | n/a | 11.2 | 5.2 | 22.2 | 2 |
| send-saturation | 1024 | `zlink-cpp` | 592.105 | 1.225 | 1.473 | 2.417 | 30.5 | 4.88 | 23.6 | 10.6 | 55.3 | 805 |
| send-saturation | 1024 | `zlink-framework-cpp` | 112.939 | 1.229 | 5.102 | 6.601 | 16.1 | 2.57 | 29.4 | 15.8 | 42.3 | 444 |
| send-saturation | 4096 | `grpc-cpp` | 23.987 | 0.169 | 0.248 | 0.287 | 12.2 | 1.95 | 26.0 | 13.7 | 31.0 | 351 |
| send-saturation | 4096 | `zlink-c` | 207.408 | 0.904 | 1.178 | 1.880 | 5.0 | n/a | 11.2 | 2.9 | 15.1 | 1 |
| send-saturation | 4096 | `zlink-cpp` | 255.994 | 1.886 | 6.638 | 7.055 | 18.1 | 2.89 | 23.4 | 10.1 | 46.4 | 635 |
| send-saturation | 4096 | `zlink-framework-cpp` | 110.435 | 1.392 | 6.242 | 9.135 | 16.5 | 2.64 | 29.6 | 16.0 | 48.2 | 440 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | saturated | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | 0 | 0.9 | 330 | no | 0.67 | submit_thread_cores | 0.278 | 1 | no | 38521 | 0 | — |
| request-serial | 1024 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | 0 | 0.9 | 339 | no | 0.65 | submit_thread_cores | 0.116 | 1 | no | 68717 | 0 | — |
| request-serial | 1024 | `zlink-framework-cpp` | 1 | 1 | 0 | 0.9 | 336 | no | 0.68 | submit_thread_cores | 0.136 | 1 | no | 44113 | 0 | — |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | 0 | 0.9 | 332 | no | 0.67 | submit_thread_cores | 0.281 | 1 | no | 36740 | 0 | — |
| request-serial | 4096 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | 0 | 0.9 | 344 | no | 0.64 | submit_thread_cores | 0.128 | 1 | no | 64365 | 0 | — |
| request-serial | 4096 | `zlink-framework-cpp` | 1 | 1 | 0 | 0.9 | 334 | no | 0.68 | submit_thread_cores | 0.133 | 1 | no | 39959 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 15783 | — | 0 | 13899.5 | 21 | no | n/a | client_cores | n/a | — | not judged | n/a | 14031 | — |
| request-backpressure | 1024 | `zlink-cpp` | 16767 (low 16532) | 0 | 0 | 13784.6 | 538 | no | 6.52 | submit_thread_cores | 4.597 | 8 | no | 773014 | 0 | — |
| request-backpressure | 1024 | `zlink-framework-cpp` | 779 (low 776) | 0 | 0 | 256.0 | 397 | no | 4.63 | submit_thread_cores | 1.477 | 8 | no | 237725 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 922 (low 770) | — | 0 | 245.3 | 1 | no | n/a | client_cores | n/a | — | not judged | n/a | 205 | — |
| request-backpressure | 4096 | `zlink-cpp` | 1114 (low 954) | 0 | 0 | 308.9 | 529 | no | 5.48 | submit_thread_cores | 2.621 | 8 | no | 707691 | 0 | — |
| request-backpressure | 4096 | `zlink-framework-cpp` | 496 (low 479) | 0 | 0 | 147.7 | 394 | no | 4.76 | submit_thread_cores | 1.514 | 8 | no | 229080 | 0 | — |
| send-saturation | 1024 | `grpc-cpp` | 8 | 1 | 0 | 4.0 | 358 | no | 1.94 | submit_thread_cores | 0.790 | 8 | no | 121965 | 0 | server |
| send-saturation | 1024 | `zlink-c` | 8 | — | 0 | 6771.5 | 2 | no | n/a | client_cores | n/a | — | not judged | 2636278 | 0 | server |
| send-saturation | 1024 | `zlink-cpp` | 8 | 1 | 0 | 725.6 | 805 | no | 4.88 | submit_thread_cores | 3.771 | 8 | no | 2960563 | 0 | server |
| send-saturation | 1024 | `zlink-framework-cpp` | 8 | 1 | 0 | 138.8 | 444 | no | 2.57 | submit_thread_cores | 1.562 | 8 | no | 564703 | 0 | server |
| send-saturation | 4096 | `grpc-cpp` | 8 | 1 | 0 | 4.0 | 351 | no | 1.95 | submit_thread_cores | 0.800 | 8 | no | 119936 | 0 | server |
| send-saturation | 4096 | `zlink-c` | 8 | — | 0 | 187.6 | 1 | no | n/a | client_cores | n/a | — | not judged | 1037039 | 0 | server |
| send-saturation | 4096 | `zlink-cpp` | 8 | 1 | 0 | 482.8 | 635 | no | 2.89 | submit_thread_cores | 1.752 | 8 | no | 1279988 | 0 | server |
| send-saturation | 4096 | `zlink-framework-cpp` | 8 | 1 | 0 | 153.7 | 440 | no | 2.64 | submit_thread_cores | 1.628 | 8 | no | 552183 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | http://127.0.0.1:5280/bench/start |
| request-serial | 1024 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | http://127.0.0.1:5285/bench/start |
| request-serial | 1024 | `zlink-framework-cpp` | 1 | 1 | http://127.0.0.1:5292/bench/start |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | http://127.0.0.1:5280/bench/start |
| request-serial | 4096 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | http://127.0.0.1:5285/bench/start |
| request-serial | 4096 | `zlink-framework-cpp` | 1 | 1 | http://127.0.0.1:5292/bench/start |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-cpp` | 8 | unbounded | http://127.0.0.1:5285/bench/start |
| request-backpressure | 1024 | `zlink-framework-cpp` | 8 | unbounded | http://127.0.0.1:5292/bench/start |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-cpp` | 8 | unbounded | http://127.0.0.1:5285/bench/start |
| request-backpressure | 4096 | `zlink-framework-cpp` | 8 | unbounded | http://127.0.0.1:5292/bench/start |
| send-saturation | 1024 | `grpc-cpp` | 8 | 1 | http://127.0.0.1:5280/bench/start |
| send-saturation | 1024 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 1024 | `zlink-cpp` | 8 | 1 | http://127.0.0.1:5285/bench/start |
| send-saturation | 1024 | `zlink-framework-cpp` | 8 | 1 | http://127.0.0.1:5292/bench/start |
| send-saturation | 4096 | `grpc-cpp` | 8 | 1 | http://127.0.0.1:5280/bench/start |
| send-saturation | 4096 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 4096 | `zlink-cpp` | 8 | 1 | http://127.0.0.1:5285/bench/start |
| send-saturation | 4096 | `zlink-framework-cpp` | 8 | 1 | http://127.0.0.1:5292/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 3 | 0.4% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 7.9% | pass |
| request-serial | 1024 | `zlink-cpp` | 3 | 2.4% | pass |
| request-serial | 1024 | `zlink-framework-cpp` | 3 | 8.2% | pass |
| request-serial | 4096 | `grpc-cpp` | 3 | 0.5% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 2.9% | pass |
| request-serial | 4096 | `zlink-cpp` | 3 | 2.4% | pass |
| request-serial | 4096 | `zlink-framework-cpp` | 3 | 7.6% | pass |
| request-backpressure | 1024 | `zlink-c` | 1 | n/a (1 run) | n/a |
| request-backpressure | 1024 | `zlink-cpp` | 3 | 1.3% | pass |
| request-backpressure | 1024 | `zlink-framework-cpp` | 3 | 1.8% | pass |
| request-backpressure | 4096 | `zlink-c` | 2 | 66.4% | n/a |
| request-backpressure | 4096 | `zlink-cpp` | 3 | 5.5% | pass |
| request-backpressure | 4096 | `zlink-framework-cpp` | 3 | 1.5% | pass |
| send-saturation | 1024 | `grpc-cpp` | 3 | 0.6% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 71.1% | **fail** |
| send-saturation | 1024 | `zlink-cpp` | 3 | 0.4% | pass |
| send-saturation | 1024 | `zlink-framework-cpp` | 3 | 1.8% | pass |
| send-saturation | 4096 | `grpc-cpp` | 3 | 0.7% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 64.0% | **fail** |
| send-saturation | 4096 | `zlink-cpp` | 3 | 44.4% | **fail** |
| send-saturation | 4096 | `zlink-framework-cpp` | 3 | 1.2% | pass |

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
| `zlink-cpp / zlink-c` | request-backpressure | 1024 | (1.191) | **unsupported** | — | denominator zlink-c-request-backpressure@1024 has 1 run(s); G5 needs 3; numerator zlink-cpp-request-backpressure@1024: warmup 2s, spec requires 30s; denominator zlink-c-request-backpressure@1024: warmup 2s, spec requires 30s (threshold 0.80) |
| `zlink-cpp / zlink-c` | request-backpressure | 4096 | (3.477) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 has 2 run(s); G5 needs 3; numerator zlink-cpp-request-backpressure@4096: warmup 2s, spec requires 30s; denominator zlink-c-request-backpressure@4096: warmup 2s, spec requires 30s (threshold 0.80) |
| `zlink-framework-cpp / zlink-cpp` | request-serial | 1024 | (0.642) | **unsupported** | — | numerator zlink-framework-cpp-request-serial@1024: warmup 2s, spec requires 30s; denominator zlink-cpp-request-serial@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | request-serial | 4096 | (0.621) | **unsupported** | — | numerator zlink-framework-cpp-request-serial@4096: warmup 2s, spec requires 30s; denominator zlink-cpp-request-serial@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | request-backpressure | 1024 | (0.313) | **unsupported** | — | numerator zlink-framework-cpp-request-backpressure@1024: warmup 2s, spec requires 30s; denominator zlink-cpp-request-backpressure@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | request-backpressure | 4096 | (0.324) | **unsupported** | — | numerator zlink-framework-cpp-request-backpressure@4096: warmup 2s, spec requires 30s; denominator zlink-cpp-request-backpressure@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | send-saturation | 1024 | (0.191) | **unsupported** | — | numerator zlink-framework-cpp-send-saturation@1024: warmup 2s, spec requires 30s; denominator zlink-cpp-send-saturation@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | send-saturation | 4096 | (0.431) | **unsupported** | — | denominator zlink-cpp-send-saturation@4096 fails G5 at 44.4% (limit 10%); numerator zlink-framework-cpp-send-saturation@4096: warmup 2s, spec requires 30s; denominator zlink-cpp-send-saturation@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**cpp: incomplete** — 8 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 6 file(s)
- run2: read cell data from 6 file(s)
- run3: read cell data from 6 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)
