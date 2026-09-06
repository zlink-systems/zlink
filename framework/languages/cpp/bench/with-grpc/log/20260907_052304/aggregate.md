## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       15.28 KOPS |   15.64 MB/s |     0.064 ms |     0.091 ms |     0.119 ms |       1.9% |    14.9 MB |       2.4% |    13.8 MB |
      | grpc-cpp                | 1024B    |       16.25 KOPS |   16.64 MB/s |     0.060 ms |     0.081 ms |     0.123 ms |       1.9% |    15.9 MB |       2.6% |    15.2 MB |
      | zlink-c                 | 1024B    |        8.29 KOPS |    8.48 MB/s |     0.120 ms |     0.139 ms |     0.170 ms |       2.3% |     7.2 MB |       1.3% |     6.6 MB |
      | zlink-cpp               | 1024B    |        8.97 KOPS |    9.19 MB/s |     0.111 ms |     0.141 ms |     0.215 ms |       2.3% |    17.0 MB |       1.4% |     7.6 MB |
      | grpc-c                  | 4096B    |       14.77 KOPS |   60.49 MB/s |     0.066 ms |     0.084 ms |     0.122 ms |       1.9% |    95.9 MB |       2.5% |    89.6 MB |
      | grpc-cpp                | 4096B    |       14.94 KOPS |   61.19 MB/s |     0.065 ms |     0.084 ms |     0.122 ms |       1.9% |    17.3 MB |       2.5% |    15.0 MB |
      | zlink-c                 | 4096B    |        8.35 KOPS |   34.20 MB/s |     0.119 ms |     0.138 ms |     0.163 ms |       2.4% |    36.4 MB |       1.6% |     8.4 MB |
      | zlink-cpp               | 4096B    |        8.79 KOPS |   36.02 MB/s |     0.113 ms |     0.147 ms |     0.206 ms |       2.3% |    17.3 MB |       1.4% |     7.6 MB |

  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       65.86 KOPS |   67.44 MB/s |     1.501 ms |     1.662 ms |     1.760 ms |       8.9% |    17.7 MB |      26.8% |    21.3 MB |
      | grpc-cpp                | 1024B    |       65.31 KOPS |   66.88 MB/s |     1.530 ms |     1.712 ms |     1.821 ms |       3.5% |    20.7 MB |      27.3% |    34.6 MB |
      | zlink-c                 | 1024B    |      428.14 KOPS |  438.41 MB/s |     0.203 ms |     0.289 ms |     0.341 ms |       8.6% |    10.0 MB |       8.9% |     7.1 MB |
      | zlink-cpp               | 1024B    |      331.44 KOPS |  339.39 MB/s |     0.301 ms |     0.353 ms |     0.408 ms |       9.5% |    21.4 MB |       8.8% |    32.8 MB |
      | grpc-c                  | 4096B    |       62.97 KOPS |  257.93 MB/s |     1.569 ms |     1.768 ms |     1.897 ms |       8.9% |    84.8 MB |      27.0% |    86.9 MB |
      | grpc-cpp                | 4096B    |       63.34 KOPS |  259.45 MB/s |     1.577 ms |     1.802 ms |     1.911 ms |       3.6% |    21.6 MB |      27.5% |    42.0 MB |
      | zlink-c                 | 4096B    |      311.80 KOPS | 1277.13 MB/s |     0.280 ms |     0.421 ms |     0.502 ms |       9.3% |    38.6 MB |       9.5% |     8.3 MB |
      | zlink-cpp               | 4096B    |      278.28 KOPS | 1139.85 MB/s |     0.359 ms |     0.412 ms |     0.447 ms |      10.4% |    22.7 MB |       9.6% |    33.6 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |     62.97 KMSG/s |   64.48 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.4% |    95.9 MB |      26.1% |    89.6 MB |
      | grpc-cpp                | 1024B    |     45.38 KMSG/s |   46.47 MB/s |     0.112 ms |     0.164 ms |     0.205 ms |       3.2% |    23.0 MB |      11.1% |    41.8 MB |
      | zlink-c                 | 1024B    |    699.35 KMSG/s |  716.13 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.1% |    35.8 MB |       4.7% |     8.3 MB |
      | zlink-cpp               | 1024B    |    696.42 KMSG/s |  713.14 MB/s |     0.443 ms |     0.987 ms |     1.100 ms |      8.2%* |    23.0 MB |       5.2% |    52.2 MB |
      | grpc-c                  | 4096B    |     61.41 KMSG/s |  251.54 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.5% |   111.1 MB |      26.3% |   106.7 MB |
      | grpc-cpp                | 4096B    |     44.51 KMSG/s |  182.33 MB/s |     0.114 ms |     0.165 ms |     0.212 ms |       3.3% |    23.2 MB |      11.0% |    42.8 MB |
      | zlink-c                 | 4096B    |    485.77 KMSG/s | 1989.70 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.7% |    36.6 MB |       6.0% |     8.3 MB |
      | zlink-cpp               | 4096B    |    514.68 KMSG/s | 2108.12 MB/s |     0.378 ms |     0.492 ms |     0.585 ms |       8.3% |    23.2 MB |       6.3% |    41.1 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-c-request-serial,local,1024,throughput,15275.000
RESULT,current,grpc-c-request-serial,local,1024,bandwidth,15.642
RESULT,current,grpc-c-request-serial,local,1024,latency,0.064
RESULT,current,grpc-c-request-serial,local,1024,latency_p95,0.091
RESULT,current,grpc-c-request-serial,local,1024,latency_p99,0.119
RESULT,current,grpc-c-request-serial,local,1024,client_cpu_percent,1.918
RESULT,current,grpc-c-request-serial,local,1024,client_memory_mb,14.859
RESULT,current,grpc-c-request-serial,local,1024,server_cpu_percent,2.410
RESULT,current,grpc-c-request-serial,local,1024,server_memory_mb,13.750
RESULT,current,grpc-cpp-request-serial,local,1024,throughput,16254.568
RESULT,current,grpc-cpp-request-serial,local,1024,bandwidth,16.645
RESULT,current,grpc-cpp-request-serial,local,1024,latency,0.060
RESULT,current,grpc-cpp-request-serial,local,1024,latency_p95,0.081
RESULT,current,grpc-cpp-request-serial,local,1024,latency_p99,0.123
RESULT,current,grpc-cpp-request-serial,local,1024,client_cpu_percent,1.853
RESULT,current,grpc-cpp-request-serial,local,1024,client_memory_mb,15.914
RESULT,current,grpc-cpp-request-serial,local,1024,server_cpu_percent,2.584
RESULT,current,grpc-cpp-request-serial,local,1024,server_memory_mb,15.156
RESULT,current,zlink-c-request-serial,local,1024,throughput,8287.000
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,8.485
RESULT,current,zlink-c-request-serial,local,1024,latency,0.120
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.139
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.170
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,2.313
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,7.234
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,1.330
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,6.562
RESULT,current,zlink-cpp-request-serial,local,1024,throughput,8972.242
RESULT,current,zlink-cpp-request-serial,local,1024,bandwidth,9.188
RESULT,current,zlink-cpp-request-serial,local,1024,latency,0.111
RESULT,current,zlink-cpp-request-serial,local,1024,latency_p95,0.141
RESULT,current,zlink-cpp-request-serial,local,1024,latency_p99,0.215
RESULT,current,zlink-cpp-request-serial,local,1024,client_cpu_percent,2.323
RESULT,current,zlink-cpp-request-serial,local,1024,client_memory_mb,16.969
RESULT,current,zlink-cpp-request-serial,local,1024,server_cpu_percent,1.411
RESULT,current,zlink-cpp-request-serial,local,1024,server_memory_mb,7.645
RESULT,current,grpc-c-request-serial,local,4096,throughput,14768.000
RESULT,current,grpc-c-request-serial,local,4096,bandwidth,60.489
RESULT,current,grpc-c-request-serial,local,4096,latency,0.066
RESULT,current,grpc-c-request-serial,local,4096,latency_p95,0.084
RESULT,current,grpc-c-request-serial,local,4096,latency_p99,0.122
RESULT,current,grpc-c-request-serial,local,4096,client_cpu_percent,1.916
RESULT,current,grpc-c-request-serial,local,4096,client_memory_mb,95.855
RESULT,current,grpc-c-request-serial,local,4096,server_cpu_percent,2.510
RESULT,current,grpc-c-request-serial,local,4096,server_memory_mb,89.594
RESULT,current,grpc-cpp-request-serial,local,4096,throughput,14939.051
RESULT,current,grpc-cpp-request-serial,local,4096,bandwidth,61.190
RESULT,current,grpc-cpp-request-serial,local,4096,latency,0.065
RESULT,current,grpc-cpp-request-serial,local,4096,latency_p95,0.084
RESULT,current,grpc-cpp-request-serial,local,4096,latency_p99,0.122
RESULT,current,grpc-cpp-request-serial,local,4096,client_cpu_percent,1.910
RESULT,current,grpc-cpp-request-serial,local,4096,client_memory_mb,17.281
RESULT,current,grpc-cpp-request-serial,local,4096,server_cpu_percent,2.511
RESULT,current,grpc-cpp-request-serial,local,4096,server_memory_mb,15.023
RESULT,current,zlink-c-request-serial,local,4096,throughput,8349.000
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,34.198
RESULT,current,zlink-c-request-serial,local,4096,latency,0.119
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.138
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.163
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,2.355
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,36.426
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,1.560
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.449
RESULT,current,zlink-cpp-request-serial,local,4096,throughput,8793.513
RESULT,current,zlink-cpp-request-serial,local,4096,bandwidth,36.018
RESULT,current,zlink-cpp-request-serial,local,4096,latency,0.113
RESULT,current,zlink-cpp-request-serial,local,4096,latency_p95,0.147
RESULT,current,zlink-cpp-request-serial,local,4096,latency_p99,0.206
RESULT,current,zlink-cpp-request-serial,local,4096,client_cpu_percent,2.334
RESULT,current,zlink-cpp-request-serial,local,4096,client_memory_mb,17.340
RESULT,current,zlink-cpp-request-serial,local,4096,server_cpu_percent,1.433
RESULT,current,zlink-cpp-request-serial,local,4096,server_memory_mb,7.637
RESULT,current,grpc-c-request-window,local,1024,throughput,65857.000
RESULT,current,grpc-c-request-window,local,1024,bandwidth,67.438
RESULT,current,grpc-c-request-window,local,1024,latency,1.501
RESULT,current,grpc-c-request-window,local,1024,latency_p95,1.662
RESULT,current,grpc-c-request-window,local,1024,latency_p99,1.760
RESULT,current,grpc-c-request-window,local,1024,client_cpu_percent,8.869
RESULT,current,grpc-c-request-window,local,1024,client_memory_mb,17.734
RESULT,current,grpc-c-request-window,local,1024,server_cpu_percent,26.838
RESULT,current,grpc-c-request-window,local,1024,server_memory_mb,21.297
RESULT,current,grpc-cpp-request-window,local,1024,throughput,65311.087
RESULT,current,grpc-cpp-request-window,local,1024,bandwidth,66.879
RESULT,current,grpc-cpp-request-window,local,1024,latency,1.530
RESULT,current,grpc-cpp-request-window,local,1024,latency_p95,1.712
RESULT,current,grpc-cpp-request-window,local,1024,latency_p99,1.821
RESULT,current,grpc-cpp-request-window,local,1024,client_cpu_percent,3.498
RESULT,current,grpc-cpp-request-window,local,1024,client_memory_mb,20.652
RESULT,current,grpc-cpp-request-window,local,1024,server_cpu_percent,27.282
RESULT,current,grpc-cpp-request-window,local,1024,server_memory_mb,34.645
RESULT,current,zlink-c-request-window,local,1024,throughput,428138.000
RESULT,current,zlink-c-request-window,local,1024,bandwidth,438.413
RESULT,current,zlink-c-request-window,local,1024,latency,0.203
RESULT,current,zlink-c-request-window,local,1024,latency_p95,0.289
RESULT,current,zlink-c-request-window,local,1024,latency_p99,0.341
RESULT,current,zlink-c-request-window,local,1024,client_cpu_percent,8.623
RESULT,current,zlink-c-request-window,local,1024,client_memory_mb,10.043
RESULT,current,zlink-c-request-window,local,1024,server_cpu_percent,8.940
RESULT,current,zlink-c-request-window,local,1024,server_memory_mb,7.129
RESULT,current,zlink-cpp-request-window,local,1024,throughput,331437.323
RESULT,current,zlink-cpp-request-window,local,1024,bandwidth,339.392
RESULT,current,zlink-cpp-request-window,local,1024,latency,0.301
RESULT,current,zlink-cpp-request-window,local,1024,latency_p95,0.353
RESULT,current,zlink-cpp-request-window,local,1024,latency_p99,0.408
RESULT,current,zlink-cpp-request-window,local,1024,client_cpu_percent,9.497
RESULT,current,zlink-cpp-request-window,local,1024,client_memory_mb,21.434
RESULT,current,zlink-cpp-request-window,local,1024,server_cpu_percent,8.768
RESULT,current,zlink-cpp-request-window,local,1024,server_memory_mb,32.824
RESULT,current,grpc-c-request-window,local,4096,throughput,62972.000
RESULT,current,grpc-c-request-window,local,4096,bandwidth,257.932
RESULT,current,grpc-c-request-window,local,4096,latency,1.569
RESULT,current,grpc-c-request-window,local,4096,latency_p95,1.768
RESULT,current,grpc-c-request-window,local,4096,latency_p99,1.897
RESULT,current,grpc-c-request-window,local,4096,client_cpu_percent,8.942
RESULT,current,grpc-c-request-window,local,4096,client_memory_mb,84.836
RESULT,current,grpc-c-request-window,local,4096,server_cpu_percent,27.008
RESULT,current,grpc-c-request-window,local,4096,server_memory_mb,86.902
RESULT,current,grpc-cpp-request-window,local,4096,throughput,63341.866
RESULT,current,grpc-cpp-request-window,local,4096,bandwidth,259.448
RESULT,current,grpc-cpp-request-window,local,4096,latency,1.577
RESULT,current,grpc-cpp-request-window,local,4096,latency_p95,1.802
RESULT,current,grpc-cpp-request-window,local,4096,latency_p99,1.911
RESULT,current,grpc-cpp-request-window,local,4096,client_cpu_percent,3.627
RESULT,current,grpc-cpp-request-window,local,4096,client_memory_mb,21.590
RESULT,current,grpc-cpp-request-window,local,4096,server_cpu_percent,27.464
RESULT,current,grpc-cpp-request-window,local,4096,server_memory_mb,42.020
RESULT,current,zlink-c-request-window,local,4096,throughput,311800.000
RESULT,current,zlink-c-request-window,local,4096,bandwidth,1277.134
RESULT,current,zlink-c-request-window,local,4096,latency,0.280
RESULT,current,zlink-c-request-window,local,4096,latency_p95,0.421
RESULT,current,zlink-c-request-window,local,4096,latency_p99,0.502
RESULT,current,zlink-c-request-window,local,4096,client_cpu_percent,9.312
RESULT,current,zlink-c-request-window,local,4096,client_memory_mb,38.613
RESULT,current,zlink-c-request-window,local,4096,server_cpu_percent,9.490
RESULT,current,zlink-c-request-window,local,4096,server_memory_mb,8.289
RESULT,current,zlink-cpp-request-window,local,4096,throughput,278283.954
RESULT,current,zlink-cpp-request-window,local,4096,bandwidth,1139.851
RESULT,current,zlink-cpp-request-window,local,4096,latency,0.359
RESULT,current,zlink-cpp-request-window,local,4096,latency_p95,0.412
RESULT,current,zlink-cpp-request-window,local,4096,latency_p99,0.447
RESULT,current,zlink-cpp-request-window,local,4096,client_cpu_percent,10.415
RESULT,current,zlink-cpp-request-window,local,4096,client_memory_mb,22.684
RESULT,current,zlink-cpp-request-window,local,4096,server_cpu_percent,9.567
RESULT,current,zlink-cpp-request-window,local,4096,server_memory_mb,33.637
RESULT,current,grpc-c-send-saturation,local,1024,throughput,62970.000
RESULT,current,grpc-c-send-saturation,local,1024,bandwidth,64.481
RESULT,current,grpc-c-send-saturation,local,1024,latency,0.000
RESULT,current,grpc-c-send-saturation,local,1024,latency_p95,0.000
RESULT,current,grpc-c-send-saturation,local,1024,latency_p99,0.000
RESULT,current,grpc-c-send-saturation,local,1024,client_cpu_percent,9.421
RESULT,current,grpc-c-send-saturation,local,1024,client_memory_mb,95.855
RESULT,current,grpc-c-send-saturation,local,1024,server_cpu_percent,26.148
RESULT,current,grpc-c-send-saturation,local,1024,server_memory_mb,89.594
RESULT,current,grpc-cpp-send-saturation,local,1024,throughput,45384.883
RESULT,current,grpc-cpp-send-saturation,local,1024,bandwidth,46.474
RESULT,current,grpc-cpp-send-saturation,local,1024,latency,0.112
RESULT,current,grpc-cpp-send-saturation,local,1024,latency_p95,0.164
RESULT,current,grpc-cpp-send-saturation,local,1024,latency_p99,0.205
RESULT,current,grpc-cpp-send-saturation,local,1024,client_cpu_percent,3.187
RESULT,current,grpc-cpp-send-saturation,local,1024,client_memory_mb,22.996
RESULT,current,grpc-cpp-send-saturation,local,1024,server_cpu_percent,11.062
RESULT,current,grpc-cpp-send-saturation,local,1024,server_memory_mb,41.758
RESULT,current,zlink-c-send-saturation,local,1024,throughput,699345.000
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,716.129
RESULT,current,zlink-c-send-saturation,local,1024,latency,0.000
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,0.000
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,0.000
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,8.141
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,35.801
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,4.730
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,8.328
RESULT,current,zlink-cpp-send-saturation,local,1024,throughput,696423.153
RESULT,current,zlink-cpp-send-saturation,local,1024,bandwidth,713.137
RESULT,current,zlink-cpp-send-saturation,local,1024,latency,0.443
RESULT,current,zlink-cpp-send-saturation,local,1024,latency_p95,0.987
RESULT,current,zlink-cpp-send-saturation,local,1024,latency_p99,1.100
RESULT,current,zlink-cpp-send-saturation,local,1024,client_cpu_percent,8.200
RESULT,current,zlink-cpp-send-saturation,local,1024,client_memory_mb,22.996
RESULT,current,zlink-cpp-send-saturation,local,1024,server_cpu_percent,5.155
RESULT,current,zlink-cpp-send-saturation,local,1024,server_memory_mb,52.230
RESULT,current,grpc-c-send-saturation,local,4096,throughput,61411.000
RESULT,current,grpc-c-send-saturation,local,4096,bandwidth,251.540
RESULT,current,grpc-c-send-saturation,local,4096,latency,0.000
RESULT,current,grpc-c-send-saturation,local,4096,latency_p95,0.000
RESULT,current,grpc-c-send-saturation,local,4096,latency_p99,0.000
RESULT,current,grpc-c-send-saturation,local,4096,client_cpu_percent,9.526
RESULT,current,grpc-c-send-saturation,local,4096,client_memory_mb,111.086
RESULT,current,grpc-c-send-saturation,local,4096,server_cpu_percent,26.309
RESULT,current,grpc-c-send-saturation,local,4096,server_memory_mb,106.707
RESULT,current,grpc-cpp-send-saturation,local,4096,throughput,44513.859
RESULT,current,grpc-cpp-send-saturation,local,4096,bandwidth,182.329
RESULT,current,grpc-cpp-send-saturation,local,4096,latency,0.114
RESULT,current,grpc-cpp-send-saturation,local,4096,latency_p95,0.165
RESULT,current,grpc-cpp-send-saturation,local,4096,latency_p99,0.212
RESULT,current,grpc-cpp-send-saturation,local,4096,client_cpu_percent,3.254
RESULT,current,grpc-cpp-send-saturation,local,4096,client_memory_mb,23.152
RESULT,current,grpc-cpp-send-saturation,local,4096,server_cpu_percent,11.014
RESULT,current,grpc-cpp-send-saturation,local,4096,server_memory_mb,42.758
RESULT,current,zlink-c-send-saturation,local,4096,throughput,485766.000
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,1989.699
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.000
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,0.000
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,0.000
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,8.749
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,36.551
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,6.020
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,8.293
RESULT,current,zlink-cpp-send-saturation,local,4096,throughput,514678.783
RESULT,current,zlink-cpp-send-saturation,local,4096,bandwidth,2108.124
RESULT,current,zlink-cpp-send-saturation,local,4096,latency,0.378
RESULT,current,zlink-cpp-send-saturation,local,4096,latency_p95,0.492
RESULT,current,zlink-cpp-send-saturation,local,4096,latency_p99,0.585
RESULT,current,zlink-cpp-send-saturation,local,4096,client_cpu_percent,8.343
RESULT,current,zlink-cpp-send-saturation,local,4096,client_memory_mb,23.152
RESULT,current,zlink-cpp-send-saturation,local,4096,server_cpu_percent,6.337
RESULT,current,zlink-cpp-send-saturation,local,4096,server_memory_mb,41.059


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Client CPU% | Client cores | Client MB | Server CPU% | Server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 15.275 | 0.064 | 0.091 | 0.119 | 1.9 | n/a | 14.9 | 2.4 | 13.8 | — |
| request-serial | 1024 | `grpc-cpp` | 16.255 | 0.060 | 0.081 | 0.123 | 1.9 | 0.37 | 15.9 | 2.6 | 15.2 | 164 |
| request-serial | 1024 | `zlink-c` | 8.287 | 0.120 | 0.139 | 0.170 | 2.3 | n/a | 7.2 | 1.3 | 6.6 | — |
| request-serial | 1024 | `zlink-cpp` | 8.972 | 0.111 | 0.141 | 0.215 | 2.3 | 0.46 | 17.0 | 1.4 | 7.6 | 158 |
| request-serial | 4096 | `grpc-c` | 14.768 | 0.066 | 0.084 | 0.122 | 1.9 | n/a | 95.9 | 2.5 | 89.6 | — |
| request-serial | 4096 | `grpc-cpp` | 14.939 | 0.065 | 0.084 | 0.122 | 1.9 | 0.38 | 17.3 | 2.5 | 15.0 | 163 |
| request-serial | 4096 | `zlink-c` | 8.349 | 0.119 | 0.138 | 0.163 | 2.4 | n/a | 36.4 | 1.6 | 8.4 | — |
| request-serial | 4096 | `zlink-cpp` | 8.794 | 0.113 | 0.147 | 0.206 | 2.3 | 0.47 | 17.3 | 1.4 | 7.6 | 158 |
| request-window | 1024 | `grpc-c` | 65.857 | 1.501 | 1.662 | 1.760 | 8.9 | n/a | 17.7 | 26.8 | 21.3 | — |
| request-window | 1024 | `grpc-cpp` | 65.311 | 1.530 | 1.712 | 1.821 | 3.5 | 0.70 | 20.7 | 27.3 | 34.6 | 214 |
| request-window | 1024 | `zlink-c` | 428.138 | 0.203 | 0.289 | 0.341 | 8.6 | n/a | 10.0 | 8.9 | 7.1 | — |
| request-window | 1024 | `zlink-cpp` | 331.437 | 0.301 | 0.353 | 0.408 | 9.5 | 1.90 | 21.4 | 8.8 | 32.8 | 445 |
| request-window | 4096 | `grpc-c` | 62.972 | 1.569 | 1.768 | 1.897 | 8.9 | n/a | 84.8 | 27.0 | 86.9 | — |
| request-window | 4096 | `grpc-cpp` | 63.342 | 1.577 | 1.802 | 1.911 | 3.6 | 0.73 | 21.6 | 27.5 | 42.0 | 211 |
| request-window | 4096 | `zlink-c` | 311.800 | 0.280 | 0.421 | 0.502 | 9.3 | n/a | 38.6 | 9.5 | 8.3 | — |
| request-window | 4096 | `zlink-cpp` | 278.284 | 0.359 | 0.412 | 0.447 | 10.4 | 2.08 | 22.7 | 9.6 | 33.6 | 398 |
| send-saturation | 1024 | `grpc-c` | 62.970 | 0.000 | 0.000 | 0.000 | 9.4 | n/a | 95.9 | 26.1 | 89.6 | — |
| send-saturation | 1024 | `grpc-cpp` | 45.385 | 0.112 | 0.164 | 0.205 | 3.2 | 0.64 | 23.0 | 11.1 | 41.8 | 195 |
| send-saturation | 1024 | `zlink-c` | 699.345 | 0.000 | 0.000 | 0.000 | 8.1 | n/a | 35.8 | 4.7 | 8.3 | — |
| send-saturation | 1024 | `zlink-cpp` | 696.423 | 0.443 | 0.987 | 1.100 | 8.2* | 1.64 | 23.0 | 5.2 | 52.2 | 494 |
| send-saturation | 4096 | `grpc-c` | 61.411 | 0.000 | 0.000 | 0.000 | 9.5 | n/a | 111.1 | 26.3 | 106.7 | — |
| send-saturation | 4096 | `grpc-cpp` | 44.514 | 0.114 | 0.165 | 0.212 | 3.3 | 0.65 | 23.2 | 11.0 | 42.8 | 194 |
| send-saturation | 4096 | `zlink-c` | 485.766 | 0.000 | 0.000 | 0.000 | 8.7 | n/a | 36.6 | 6.0 | 8.3 | — |
| send-saturation | 4096 | `zlink-cpp` | 514.679 | 0.378 | 0.492 | 0.585 | 8.3 | 1.67 | 23.2 | 6.3 | 41.1 | 513 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | client cores | saturation metric | reading | declared ceiling | saturated | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | 0 | 1.0 | 164 | no | 0.37 | submit_thread_cores | 0.368 | 1 | no | — |
| request-serial | 1024 | `zlink-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | 0 | 1.0 | 158 | no | 0.46 | submit_thread_cores | 0.182 | 1 | no | — |
| request-serial | 4096 | `grpc-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | 0 | 1.0 | 163 | no | 0.38 | submit_thread_cores | 0.382 | 1 | no | — |
| request-serial | 4096 | `zlink-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | 0 | 1.0 | 158 | no | 0.47 | submit_thread_cores | 0.180 | 1 | no | — |
| request-window | 1024 | `grpc-c` | 100 | — | — | 98.9 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 1024 | `grpc-cpp` | 100 | 100 | 0 | 99.9 | 214 | no | 0.70 | submit_thread_cores | 0.700 | 1 | no | — |
| request-window | 1024 | `zlink-c` | 100 | — | — | 86.9 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 1024 | `zlink-cpp` | 100 | 100 | 0 | 99.9 | 445 | no | 1.90 | submit_thread_cores | 0.950 | 1 | no | — |
| request-window | 4096 | `grpc-c` | 100 | — | — | 98.8 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 4096 | `grpc-cpp` | 100 | 100 | 0 | 99.9 | 211 | no | 0.73 | submit_thread_cores | 0.726 | 1 | no | — |
| request-window | 4096 | `zlink-c` | 100 | — | — | 87.3 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 4096 | `zlink-cpp` | 100 | 100 | 0 | 99.8 | 398 | no | 2.08 | submit_thread_cores | 0.928 | 1 | no | — |
| send-saturation | 1024 | `grpc-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 1024 | `grpc-cpp` | 8 | 8 | 0 | 5.1 | 195 | no | 0.64 | submit_thread_cores | 0.638 | 1 | no | server |
| send-saturation | 1024 | `zlink-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 1024 | `zlink-cpp` | 8 | 8 | 0 | 308.8 | 494 | no | 1.64 | submit_thread_cores | 0.964 | 1 | **yes** | server |
| send-saturation | 4096 | `grpc-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 4096 | `grpc-cpp` | 8 | 8 | 0 | 5.1 | 194 | no | 0.65 | submit_thread_cores | 0.650 | 1 | no | server |
| send-saturation | 4096 | `zlink-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 4096 | `zlink-cpp` | 8 | 8 | 0 | 194.4 | 513 | no | 1.67 | submit_thread_cores | 0.878 | 1 | no | server |

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 3 | 4.4% | pass |
| request-serial | 1024 | `grpc-cpp` | 3 | 2.3% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 37.9% | **fail** |
| request-serial | 1024 | `zlink-cpp` | 3 | 0.7% | pass |
| request-serial | 4096 | `grpc-c` | 3 | 0.5% | pass |
| request-serial | 4096 | `grpc-cpp` | 3 | 2.6% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 12.6% | **fail** |
| request-serial | 4096 | `zlink-cpp` | 3 | 2.3% | pass |
| request-window | 1024 | `grpc-c` | 3 | 1.0% | pass |
| request-window | 1024 | `grpc-cpp` | 3 | 1.4% | pass |
| request-window | 1024 | `zlink-c` | 3 | 7.4% | pass |
| request-window | 1024 | `zlink-cpp` | 3 | 0.4% | pass |
| request-window | 4096 | `grpc-c` | 3 | 2.6% | pass |
| request-window | 4096 | `grpc-cpp` | 3 | 1.5% | pass |
| request-window | 4096 | `zlink-c` | 3 | 28.7% | **fail** |
| request-window | 4096 | `zlink-cpp` | 3 | 1.4% | pass |
| send-saturation | 1024 | `grpc-c` | 3 | 3.3% | pass |
| send-saturation | 1024 | `grpc-cpp` | 3 | 0.9% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 2.9% | pass |
| send-saturation | 1024 | `zlink-cpp` | 3 | 2.8% | pass |
| send-saturation | 4096 | `grpc-c` | 3 | 1.1% | pass |
| send-saturation | 4096 | `grpc-cpp` | 3 | 0.4% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 1.0% | pass |
| send-saturation | 4096 | `zlink-cpp` | 3 | 3.8% | pass |

## Contaminated cells (FB-008, excluded from tables and judgement)

None. Every cell drained within the bound.

## Judgement (spec 7.2, FB-005, FB-011)

| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-cpp / zlink-c` | 1024 | 0.774 | published | **fail** | both rows pass G5 |
| `zlink-cpp / zlink-c` | 4096 | (0.893) | **unsupported** | — | denominator zlink-c-request-window@4096 fails G5 at 28.7% (limit 10%) |
| `zlink-framework-cpp / zlink-cpp` | 1024 | n/a | **unsupported** | — | numerator zlink-framework-cpp-request-window@1024 was not measured |
| `zlink-framework-cpp / zlink-cpp` | 4096 | n/a | **unsupported** | — | numerator zlink-framework-cpp-request-window@4096 was not measured |

**cpp: incomplete** — 3 of 4 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- cpp-router-1: read structured with-grpc-cell-v1
- cpp-router-2: read structured with-grpc-cell-v1
- cpp-router-3: read structured with-grpc-cell-v1
- c-router-1: throughput read as KOPS (scale 1000)
- c-router-1: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- c-router-1: no client parallelism ceiling declared; spec 5.1 saturation not judged
- c-router-2: throughput read as KOPS (scale 1000)
- c-router-2: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- c-router-2: no client parallelism ceiling declared; spec 5.1 saturation not judged
- c-router-3: throughput read as KOPS (scale 1000)
- c-router-3: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- c-router-3: no client parallelism ceiling declared; spec 5.1 saturation not judged
