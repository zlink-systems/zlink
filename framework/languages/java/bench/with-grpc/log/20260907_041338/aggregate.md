## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       15.28 KOPS |   15.64 MB/s |     0.064 ms |     0.091 ms |     0.119 ms |       1.9% |    14.9 MB |       2.4% |    13.8 MB |
      | grpc-kotlin             | 1024B    |        4.74 KOPS |    4.86 MB/s |     0.211 ms |     0.261 ms |     0.324 ms |       3.0% |   341.1 MB |       1.6% |   750.4 MB |
      | zlink-c                 | 1024B    |        8.29 KOPS |    8.48 MB/s |     0.120 ms |     0.139 ms |     0.170 ms |       2.3% |     7.2 MB |       1.3% |     6.6 MB |
      | zlink-framework-kotlin  | 1024B    |        0.47 KOPS |    0.48 MB/s |     2.121 ms |     2.589 ms |     2.783 ms |       1.7% |   404.4 MB |       2.1% |   388.5 MB |
      | zlink-kotlin            | 1024B    |        5.32 KOPS |    5.45 MB/s |     0.188 ms |     0.230 ms |     0.292 ms |       2.6% |   361.1 MB |       1.2% |   399.6 MB |
      | grpc-c                  | 4096B    |       14.77 KOPS |   60.49 MB/s |     0.066 ms |     0.084 ms |     0.122 ms |       1.9% |    95.9 MB |       2.5% |    89.6 MB |
      | grpc-kotlin             | 4096B    |        4.89 KOPS |   20.02 MB/s |     0.204 ms |     0.241 ms |     0.308 ms |       3.0% |  1133.5 MB |       1.5% |  1235.0 MB |
      | zlink-c                 | 4096B    |        8.35 KOPS |   34.20 MB/s |     0.119 ms |     0.138 ms |     0.163 ms |       2.4% |    36.4 MB |       1.6% |     8.4 MB |
      | zlink-framework-kotlin  | 4096B    |        0.49 KOPS |    2.02 MB/s |     2.032 ms |     2.505 ms |     2.578 ms |       1.7% |  1134.2 MB |       1.4% |   884.8 MB |
      | zlink-kotlin            | 4096B    |        5.40 KOPS |   22.12 MB/s |     0.185 ms |     0.223 ms |     0.297 ms |       2.7% |  1136.2 MB |       1.2% |  1039.7 MB |

  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       65.86 KOPS |   67.44 MB/s |     1.501 ms |     1.662 ms |     1.760 ms |       8.9% |    17.7 MB |      26.8% |    21.3 MB |
      | grpc-kotlin             | 1024B    |       93.75 KOPS |   96.00 MB/s |     0.982 ms |     1.239 ms |     1.818 ms |      28.2% |   943.5 MB |      12.3% |  1206.7 MB |
      | zlink-c                 | 1024B    |      428.14 KOPS |  438.41 MB/s |     0.203 ms |     0.289 ms |     0.341 ms |       8.6% |    10.0 MB |       8.9% |     7.1 MB |
      | zlink-framework-kotlin  | 1024B    |        2.36 KOPS |    2.42 MB/s |     1.920 ms |     2.769 ms |     2.994 ms |       5.3% |   996.4 MB |       4.8% |   378.6 MB |
      | zlink-kotlin            | 1024B    |        0.00 KOPS |    0.00 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       0.6% |   946.9 MB |       0.0% |   404.6 MB |
      | grpc-c                  | 4096B    |       62.97 KOPS |  257.93 MB/s |     1.569 ms |     1.768 ms |     1.897 ms |       8.9% |    84.8 MB |      27.0% |    86.9 MB |
      | grpc-kotlin             | 4096B    |       79.46 KOPS |  325.47 MB/s |     1.172 ms |     1.555 ms |     1.851 ms |      24.4% |  1550.8 MB |      11.5% |  1257.3 MB |
      | zlink-c                 | 4096B    |      311.80 KOPS | 1277.13 MB/s |     0.280 ms |     0.421 ms |     0.502 ms |       9.3% |    38.6 MB |       9.5% |     8.3 MB |
      | zlink-framework-kotlin  | 4096B    |        2.30 KOPS |    9.41 MB/s |     1.952 ms |     2.800 ms |     3.020 ms |       5.5% |  1557.7 MB |       4.9% |   897.7 MB |
      | zlink-kotlin            | 4096B    |        0.00 KOPS |    0.00 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       0.7% |  1555.4 MB |       0.0% |  1040.0 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem | Server CPU | Server Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |     62.97 KMSG/s |   64.48 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.4% |    95.9 MB |      26.1% |    89.6 MB |
      | grpc-kotlin             | 1024B    |     35.18 KMSG/s |   36.02 MB/s |     0.120 ms |     0.167 ms |     0.194 ms |      13.8% |  1033.6 MB |       6.6% |  1234.8 MB |
      | zlink-c                 | 1024B    |    699.35 KMSG/s |  716.13 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.1% |    35.8 MB |       4.7% |     8.3 MB |
      | zlink-framework-kotlin  | 1024B    |      5.67 KMSG/s |    5.80 MB/s |  2482.409 ms |  2655.419 ms |  2675.103 ms |      13.1% |  1132.7 MB |      13.8% |   847.2 MB |
      | zlink-kotlin            | 1024B    |    372.46 KMSG/s |  381.40 MB/s |     0.076 ms |     0.232 ms |     0.291 ms |      10.1% |  1047.7 MB |       4.2% |  1036.0 MB |
      | grpc-c                  | 4096B    |     61.41 KMSG/s |  251.54 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.5% |   111.1 MB |      26.3% |   106.7 MB |
      | grpc-kotlin             | 4096B    |     33.75 KMSG/s |  138.25 MB/s |     0.126 ms |     0.177 ms |     0.206 ms |      13.7% |  1573.5 MB |       6.5% |  1256.9 MB |
      | zlink-c                 | 4096B    |    485.77 KMSG/s | 1989.70 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.7% |    36.6 MB |       6.0% |     8.3 MB |
      | zlink-framework-kotlin  | 4096B    |      8.67 KMSG/s |   35.52 MB/s |   819.060 ms |   897.205 ms |   918.998 ms |      13.3% |  1614.5 MB |      13.4% |   907.2 MB |
      | zlink-kotlin            | 4096B    |    256.76 KMSG/s | 1051.67 MB/s |     0.084 ms |     0.233 ms |     0.285 ms |      10.4% |  1576.0 MB |       4.4% |  1041.2 MB |

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
RESULT,current,grpc-kotlin-request-serial,local,1024,throughput,4744.200
RESULT,current,grpc-kotlin-request-serial,local,1024,bandwidth,4.858
RESULT,current,grpc-kotlin-request-serial,local,1024,latency,0.211
RESULT,current,grpc-kotlin-request-serial,local,1024,latency_p95,0.261
RESULT,current,grpc-kotlin-request-serial,local,1024,latency_p99,0.324
RESULT,current,grpc-kotlin-request-serial,local,1024,client_cpu_percent,3.020
RESULT,current,grpc-kotlin-request-serial,local,1024,client_memory_mb,341.145
RESULT,current,grpc-kotlin-request-serial,local,1024,server_cpu_percent,1.650
RESULT,current,grpc-kotlin-request-serial,local,1024,server_memory_mb,750.395
RESULT,current,zlink-c-request-serial,local,1024,throughput,8287.000
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,8.485
RESULT,current,zlink-c-request-serial,local,1024,latency,0.120
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.139
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.170
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,2.313
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,7.234
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,1.330
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,6.562
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,throughput,471.600
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,bandwidth,0.483
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,latency,2.121
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,latency_p95,2.589
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,latency_p99,2.783
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,client_cpu_percent,1.730
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,client_memory_mb,404.434
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,server_cpu_percent,2.140
RESULT,current,zlink-framework-kotlin-request-serial,local,1024,server_memory_mb,388.492
RESULT,current,zlink-kotlin-request-serial,local,1024,throughput,5322.400
RESULT,current,zlink-kotlin-request-serial,local,1024,bandwidth,5.450
RESULT,current,zlink-kotlin-request-serial,local,1024,latency,0.188
RESULT,current,zlink-kotlin-request-serial,local,1024,latency_p95,0.230
RESULT,current,zlink-kotlin-request-serial,local,1024,latency_p99,0.292
RESULT,current,zlink-kotlin-request-serial,local,1024,client_cpu_percent,2.580
RESULT,current,zlink-kotlin-request-serial,local,1024,client_memory_mb,361.098
RESULT,current,zlink-kotlin-request-serial,local,1024,server_cpu_percent,1.230
RESULT,current,zlink-kotlin-request-serial,local,1024,server_memory_mb,399.559
RESULT,current,grpc-c-request-serial,local,4096,throughput,14768.000
RESULT,current,grpc-c-request-serial,local,4096,bandwidth,60.489
RESULT,current,grpc-c-request-serial,local,4096,latency,0.066
RESULT,current,grpc-c-request-serial,local,4096,latency_p95,0.084
RESULT,current,grpc-c-request-serial,local,4096,latency_p99,0.122
RESULT,current,grpc-c-request-serial,local,4096,client_cpu_percent,1.916
RESULT,current,grpc-c-request-serial,local,4096,client_memory_mb,95.855
RESULT,current,grpc-c-request-serial,local,4096,server_cpu_percent,2.510
RESULT,current,grpc-c-request-serial,local,4096,server_memory_mb,89.594
RESULT,current,grpc-kotlin-request-serial,local,4096,throughput,4887.400
RESULT,current,grpc-kotlin-request-serial,local,4096,bandwidth,20.019
RESULT,current,grpc-kotlin-request-serial,local,4096,latency,0.204
RESULT,current,grpc-kotlin-request-serial,local,4096,latency_p95,0.241
RESULT,current,grpc-kotlin-request-serial,local,4096,latency_p99,0.308
RESULT,current,grpc-kotlin-request-serial,local,4096,client_cpu_percent,3.030
RESULT,current,grpc-kotlin-request-serial,local,4096,client_memory_mb,1133.496
RESULT,current,grpc-kotlin-request-serial,local,4096,server_cpu_percent,1.520
RESULT,current,grpc-kotlin-request-serial,local,4096,server_memory_mb,1234.988
RESULT,current,zlink-c-request-serial,local,4096,throughput,8349.000
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,34.198
RESULT,current,zlink-c-request-serial,local,4096,latency,0.119
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.138
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.163
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,2.355
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,36.426
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,1.560
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.449
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,throughput,492.400
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,bandwidth,2.017
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,latency,2.032
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,latency_p95,2.505
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,latency_p99,2.578
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,client_cpu_percent,1.719
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,client_memory_mb,1134.250
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,server_cpu_percent,1.429
RESULT,current,zlink-framework-kotlin-request-serial,local,4096,server_memory_mb,884.844
RESULT,current,zlink-kotlin-request-serial,local,4096,throughput,5401.600
RESULT,current,zlink-kotlin-request-serial,local,4096,bandwidth,22.125
RESULT,current,zlink-kotlin-request-serial,local,4096,latency,0.185
RESULT,current,zlink-kotlin-request-serial,local,4096,latency_p95,0.223
RESULT,current,zlink-kotlin-request-serial,local,4096,latency_p99,0.297
RESULT,current,zlink-kotlin-request-serial,local,4096,client_cpu_percent,2.680
RESULT,current,zlink-kotlin-request-serial,local,4096,client_memory_mb,1136.152
RESULT,current,zlink-kotlin-request-serial,local,4096,server_cpu_percent,1.170
RESULT,current,zlink-kotlin-request-serial,local,4096,server_memory_mb,1039.715
RESULT,current,grpc-c-request-window,local,1024,throughput,65857.000
RESULT,current,grpc-c-request-window,local,1024,bandwidth,67.438
RESULT,current,grpc-c-request-window,local,1024,latency,1.501
RESULT,current,grpc-c-request-window,local,1024,latency_p95,1.662
RESULT,current,grpc-c-request-window,local,1024,latency_p99,1.760
RESULT,current,grpc-c-request-window,local,1024,client_cpu_percent,8.869
RESULT,current,grpc-c-request-window,local,1024,client_memory_mb,17.734
RESULT,current,grpc-c-request-window,local,1024,server_cpu_percent,26.838
RESULT,current,grpc-c-request-window,local,1024,server_memory_mb,21.297
RESULT,current,grpc-kotlin-request-window,local,1024,throughput,93750.800
RESULT,current,grpc-kotlin-request-window,local,1024,bandwidth,96.001
RESULT,current,grpc-kotlin-request-window,local,1024,latency,0.982
RESULT,current,grpc-kotlin-request-window,local,1024,latency_p95,1.239
RESULT,current,grpc-kotlin-request-window,local,1024,latency_p99,1.818
RESULT,current,grpc-kotlin-request-window,local,1024,client_cpu_percent,28.164
RESULT,current,grpc-kotlin-request-window,local,1024,client_memory_mb,943.496
RESULT,current,grpc-kotlin-request-window,local,1024,server_cpu_percent,12.287
RESULT,current,grpc-kotlin-request-window,local,1024,server_memory_mb,1206.656
RESULT,current,zlink-c-request-window,local,1024,throughput,428138.000
RESULT,current,zlink-c-request-window,local,1024,bandwidth,438.413
RESULT,current,zlink-c-request-window,local,1024,latency,0.203
RESULT,current,zlink-c-request-window,local,1024,latency_p95,0.289
RESULT,current,zlink-c-request-window,local,1024,latency_p99,0.341
RESULT,current,zlink-c-request-window,local,1024,client_cpu_percent,8.623
RESULT,current,zlink-c-request-window,local,1024,client_memory_mb,10.043
RESULT,current,zlink-c-request-window,local,1024,server_cpu_percent,8.940
RESULT,current,zlink-c-request-window,local,1024,server_memory_mb,7.129
RESULT,current,zlink-framework-kotlin-request-window,local,1024,throughput,2360.000
RESULT,current,zlink-framework-kotlin-request-window,local,1024,bandwidth,2.417
RESULT,current,zlink-framework-kotlin-request-window,local,1024,latency,1.920
RESULT,current,zlink-framework-kotlin-request-window,local,1024,latency_p95,2.769
RESULT,current,zlink-framework-kotlin-request-window,local,1024,latency_p99,2.994
RESULT,current,zlink-framework-kotlin-request-window,local,1024,client_cpu_percent,5.338
RESULT,current,zlink-framework-kotlin-request-window,local,1024,client_memory_mb,996.398
RESULT,current,zlink-framework-kotlin-request-window,local,1024,server_cpu_percent,4.778
RESULT,current,zlink-framework-kotlin-request-window,local,1024,server_memory_mb,378.574
RESULT,current,zlink-kotlin-request-window,local,1024,throughput,0.000
RESULT,current,zlink-kotlin-request-window,local,1024,bandwidth,0.000
RESULT,current,zlink-kotlin-request-window,local,1024,latency,0.000
RESULT,current,zlink-kotlin-request-window,local,1024,latency_p95,0.000
RESULT,current,zlink-kotlin-request-window,local,1024,latency_p99,0.000
RESULT,current,zlink-kotlin-request-window,local,1024,client_cpu_percent,0.650
RESULT,current,zlink-kotlin-request-window,local,1024,client_memory_mb,946.867
RESULT,current,zlink-kotlin-request-window,local,1024,server_cpu_percent,0.010
RESULT,current,zlink-kotlin-request-window,local,1024,server_memory_mb,404.559
RESULT,current,grpc-c-request-window,local,4096,throughput,62972.000
RESULT,current,grpc-c-request-window,local,4096,bandwidth,257.932
RESULT,current,grpc-c-request-window,local,4096,latency,1.569
RESULT,current,grpc-c-request-window,local,4096,latency_p95,1.768
RESULT,current,grpc-c-request-window,local,4096,latency_p99,1.897
RESULT,current,grpc-c-request-window,local,4096,client_cpu_percent,8.942
RESULT,current,grpc-c-request-window,local,4096,client_memory_mb,84.836
RESULT,current,grpc-c-request-window,local,4096,server_cpu_percent,27.008
RESULT,current,grpc-c-request-window,local,4096,server_memory_mb,86.902
RESULT,current,grpc-kotlin-request-window,local,4096,throughput,79459.400
RESULT,current,grpc-kotlin-request-window,local,4096,bandwidth,325.466
RESULT,current,grpc-kotlin-request-window,local,4096,latency,1.172
RESULT,current,grpc-kotlin-request-window,local,4096,latency_p95,1.555
RESULT,current,grpc-kotlin-request-window,local,4096,latency_p99,1.851
RESULT,current,grpc-kotlin-request-window,local,4096,client_cpu_percent,24.355
RESULT,current,grpc-kotlin-request-window,local,4096,client_memory_mb,1550.812
RESULT,current,grpc-kotlin-request-window,local,4096,server_cpu_percent,11.457
RESULT,current,grpc-kotlin-request-window,local,4096,server_memory_mb,1257.262
RESULT,current,zlink-c-request-window,local,4096,throughput,311800.000
RESULT,current,zlink-c-request-window,local,4096,bandwidth,1277.134
RESULT,current,zlink-c-request-window,local,4096,latency,0.280
RESULT,current,zlink-c-request-window,local,4096,latency_p95,0.421
RESULT,current,zlink-c-request-window,local,4096,latency_p99,0.502
RESULT,current,zlink-c-request-window,local,4096,client_cpu_percent,9.312
RESULT,current,zlink-c-request-window,local,4096,client_memory_mb,38.613
RESULT,current,zlink-c-request-window,local,4096,server_cpu_percent,9.490
RESULT,current,zlink-c-request-window,local,4096,server_memory_mb,8.289
RESULT,current,zlink-framework-kotlin-request-window,local,4096,throughput,2296.800
RESULT,current,zlink-framework-kotlin-request-window,local,4096,bandwidth,9.408
RESULT,current,zlink-framework-kotlin-request-window,local,4096,latency,1.952
RESULT,current,zlink-framework-kotlin-request-window,local,4096,latency_p95,2.800
RESULT,current,zlink-framework-kotlin-request-window,local,4096,latency_p99,3.020
RESULT,current,zlink-framework-kotlin-request-window,local,4096,client_cpu_percent,5.539
RESULT,current,zlink-framework-kotlin-request-window,local,4096,client_memory_mb,1557.727
RESULT,current,zlink-framework-kotlin-request-window,local,4096,server_cpu_percent,4.889
RESULT,current,zlink-framework-kotlin-request-window,local,4096,server_memory_mb,897.656
RESULT,current,zlink-kotlin-request-window,local,4096,throughput,0.000
RESULT,current,zlink-kotlin-request-window,local,4096,bandwidth,0.000
RESULT,current,zlink-kotlin-request-window,local,4096,latency,0.000
RESULT,current,zlink-kotlin-request-window,local,4096,latency_p95,0.000
RESULT,current,zlink-kotlin-request-window,local,4096,latency_p99,0.000
RESULT,current,zlink-kotlin-request-window,local,4096,client_cpu_percent,0.665
RESULT,current,zlink-kotlin-request-window,local,4096,client_memory_mb,1555.445
RESULT,current,zlink-kotlin-request-window,local,4096,server_cpu_percent,0.010
RESULT,current,zlink-kotlin-request-window,local,4096,server_memory_mb,1040.027
RESULT,current,grpc-c-send-saturation,local,1024,throughput,62970.000
RESULT,current,grpc-c-send-saturation,local,1024,bandwidth,64.481
RESULT,current,grpc-c-send-saturation,local,1024,latency,0.000
RESULT,current,grpc-c-send-saturation,local,1024,latency_p95,0.000
RESULT,current,grpc-c-send-saturation,local,1024,latency_p99,0.000
RESULT,current,grpc-c-send-saturation,local,1024,client_cpu_percent,9.421
RESULT,current,grpc-c-send-saturation,local,1024,client_memory_mb,95.855
RESULT,current,grpc-c-send-saturation,local,1024,server_cpu_percent,26.148
RESULT,current,grpc-c-send-saturation,local,1024,server_memory_mb,89.594
RESULT,current,grpc-kotlin-send-saturation,local,1024,throughput,35175.800
RESULT,current,grpc-kotlin-send-saturation,local,1024,bandwidth,36.020
RESULT,current,grpc-kotlin-send-saturation,local,1024,latency,0.120
RESULT,current,grpc-kotlin-send-saturation,local,1024,latency_p95,0.167
RESULT,current,grpc-kotlin-send-saturation,local,1024,latency_p99,0.194
RESULT,current,grpc-kotlin-send-saturation,local,1024,client_cpu_percent,13.769
RESULT,current,grpc-kotlin-send-saturation,local,1024,client_memory_mb,1033.605
RESULT,current,grpc-kotlin-send-saturation,local,1024,server_cpu_percent,6.650
RESULT,current,grpc-kotlin-send-saturation,local,1024,server_memory_mb,1234.832
RESULT,current,zlink-c-send-saturation,local,1024,throughput,699345.000
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,716.129
RESULT,current,zlink-c-send-saturation,local,1024,latency,0.000
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,0.000
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,0.000
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,8.141
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,35.801
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,4.730
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,8.328
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,throughput,5668.400
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,bandwidth,5.804
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,latency,2482.409
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,latency_p95,2655.419
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,latency_p99,2675.103
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,client_cpu_percent,13.109
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,client_memory_mb,1132.715
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,server_cpu_percent,13.809
RESULT,current,zlink-framework-kotlin-send-saturation,local,1024,server_memory_mb,847.227
RESULT,current,zlink-kotlin-send-saturation,local,1024,throughput,372464.400
RESULT,current,zlink-kotlin-send-saturation,local,1024,bandwidth,381.404
RESULT,current,zlink-kotlin-send-saturation,local,1024,latency,0.076
RESULT,current,zlink-kotlin-send-saturation,local,1024,latency_p95,0.232
RESULT,current,zlink-kotlin-send-saturation,local,1024,latency_p99,0.291
RESULT,current,zlink-kotlin-send-saturation,local,1024,client_cpu_percent,10.150
RESULT,current,zlink-kotlin-send-saturation,local,1024,client_memory_mb,1047.684
RESULT,current,zlink-kotlin-send-saturation,local,1024,server_cpu_percent,4.240
RESULT,current,zlink-kotlin-send-saturation,local,1024,server_memory_mb,1035.965
RESULT,current,grpc-c-send-saturation,local,4096,throughput,61411.000
RESULT,current,grpc-c-send-saturation,local,4096,bandwidth,251.540
RESULT,current,grpc-c-send-saturation,local,4096,latency,0.000
RESULT,current,grpc-c-send-saturation,local,4096,latency_p95,0.000
RESULT,current,grpc-c-send-saturation,local,4096,latency_p99,0.000
RESULT,current,grpc-c-send-saturation,local,4096,client_cpu_percent,9.526
RESULT,current,grpc-c-send-saturation,local,4096,client_memory_mb,111.086
RESULT,current,grpc-c-send-saturation,local,4096,server_cpu_percent,26.309
RESULT,current,grpc-c-send-saturation,local,4096,server_memory_mb,106.707
RESULT,current,grpc-kotlin-send-saturation,local,4096,throughput,33752.200
RESULT,current,grpc-kotlin-send-saturation,local,4096,bandwidth,138.249
RESULT,current,grpc-kotlin-send-saturation,local,4096,latency,0.126
RESULT,current,grpc-kotlin-send-saturation,local,4096,latency_p95,0.177
RESULT,current,grpc-kotlin-send-saturation,local,4096,latency_p99,0.206
RESULT,current,grpc-kotlin-send-saturation,local,4096,client_cpu_percent,13.719
RESULT,current,grpc-kotlin-send-saturation,local,4096,client_memory_mb,1573.535
RESULT,current,grpc-kotlin-send-saturation,local,4096,server_cpu_percent,6.500
RESULT,current,grpc-kotlin-send-saturation,local,4096,server_memory_mb,1256.863
RESULT,current,zlink-c-send-saturation,local,4096,throughput,485766.000
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,1989.699
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.000
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,0.000
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,0.000
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,8.749
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,36.551
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,6.020
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,8.293
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,throughput,8670.800
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,bandwidth,35.516
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,latency,819.060
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,latency_p95,897.205
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,latency_p99,918.998
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,client_cpu_percent,13.309
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,client_memory_mb,1614.473
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,server_cpu_percent,13.419
RESULT,current,zlink-framework-kotlin-send-saturation,local,4096,server_memory_mb,907.188
RESULT,current,zlink-kotlin-send-saturation,local,4096,throughput,256755.400
RESULT,current,zlink-kotlin-send-saturation,local,4096,bandwidth,1051.670
RESULT,current,zlink-kotlin-send-saturation,local,4096,latency,0.084
RESULT,current,zlink-kotlin-send-saturation,local,4096,latency_p95,0.233
RESULT,current,zlink-kotlin-send-saturation,local,4096,latency_p99,0.285
RESULT,current,zlink-kotlin-send-saturation,local,4096,client_cpu_percent,10.369
RESULT,current,zlink-kotlin-send-saturation,local,4096,client_memory_mb,1576.035
RESULT,current,zlink-kotlin-send-saturation,local,4096,server_cpu_percent,4.430
RESULT,current,zlink-kotlin-send-saturation,local,4096,server_memory_mb,1041.199


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Client CPU% | Client cores | Client MB | Server CPU% | Server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 15.275 | 0.064 | 0.091 | 0.119 | 1.9 | n/a | 14.9 | 2.4 | 13.8 | — |
| request-serial | 1024 | `grpc-kotlin` | 4.744 | 0.211 | 0.261 | 0.324 | 3.0 | 0.60 | 341.1 | 1.6 | 750.4 | — |
| request-serial | 1024 | `zlink-c` | 8.287 | 0.120 | 0.139 | 0.170 | 2.3 | n/a | 7.2 | 1.3 | 6.6 | — |
| request-serial | 1024 | `zlink-framework-kotlin` | 0.472 | 2.121 | 2.589 | 2.783 | 1.7 | 0.35 | 404.4 | 2.1 | 388.5 | — |
| request-serial | 1024 | `zlink-kotlin` | 5.322 | 0.188 | 0.230 | 0.292 | 2.6 | 0.52 | 361.1 | 1.2 | 399.6 | — |
| request-serial | 4096 | `grpc-c` | 14.768 | 0.066 | 0.084 | 0.122 | 1.9 | n/a | 95.9 | 2.5 | 89.6 | — |
| request-serial | 4096 | `grpc-kotlin` | 4.887 | 0.204 | 0.241 | 0.308 | 3.0 | 0.61 | 1133.5 | 1.5 | 1235.0 | — |
| request-serial | 4096 | `zlink-c` | 8.349 | 0.119 | 0.138 | 0.163 | 2.4 | n/a | 36.4 | 1.6 | 8.4 | — |
| request-serial | 4096 | `zlink-framework-kotlin` | 0.492 | 2.032 | 2.505 | 2.578 | 1.7 | 0.34 | 1134.2 | 1.4 | 884.8 | — |
| request-serial | 4096 | `zlink-kotlin` | 5.402 | 0.185 | 0.223 | 0.297 | 2.7 | 0.54 | 1136.2 | 1.2 | 1039.7 | — |
| request-window | 1024 | `grpc-c` | 65.857 | 1.501 | 1.662 | 1.760 | 8.9 | n/a | 17.7 | 26.8 | 21.3 | — |
| request-window | 1024 | `grpc-kotlin` | 93.751 | 0.982 | 1.239 | 1.818 | 28.2 | 5.63 | 943.5 | 12.3 | 1206.7 | — |
| request-window | 1024 | `zlink-c` | 428.138 | 0.203 | 0.289 | 0.341 | 8.6 | n/a | 10.0 | 8.9 | 7.1 | — |
| request-window | 1024 | `zlink-framework-kotlin` | 2.360 | 1.920 | 2.769 | 2.994 | 5.3 | 1.07 | 996.4 | 4.8 | 378.6 | — |
| request-window | 1024 | `zlink-kotlin` | 0.000 | 0.000 | 0.000 | 0.000 | 0.6 | 0.13 | 946.9 | 0.0 | 404.6 | — |
| request-window | 4096 | `grpc-c` | 62.972 | 1.569 | 1.768 | 1.897 | 8.9 | n/a | 84.8 | 27.0 | 86.9 | — |
| request-window | 4096 | `grpc-kotlin` | 79.459 | 1.172 | 1.555 | 1.851 | 24.4 | 4.87 | 1550.8 | 11.5 | 1257.3 | — |
| request-window | 4096 | `zlink-c` | 311.800 | 0.280 | 0.421 | 0.502 | 9.3 | n/a | 38.6 | 9.5 | 8.3 | — |
| request-window | 4096 | `zlink-framework-kotlin` | 2.297 | 1.952 | 2.800 | 3.020 | 5.5 | 1.11 | 1557.7 | 4.9 | 897.7 | — |
| request-window | 4096 | `zlink-kotlin` | 0.000 | 0.000 | 0.000 | 0.000 | 0.7 | 0.13 | 1555.4 | 0.0 | 1040.0 | — |
| send-saturation | 1024 | `grpc-c` | 62.970 | 0.000 | 0.000 | 0.000 | 9.4 | n/a | 95.9 | 26.1 | 89.6 | — |
| send-saturation | 1024 | `grpc-kotlin` | 35.176 | 0.120 | 0.167 | 0.194 | 13.8 | 2.75 | 1033.6 | 6.6 | 1234.8 | 310 |
| send-saturation | 1024 | `zlink-c` | 699.345 | 0.000 | 0.000 | 0.000 | 8.1 | n/a | 35.8 | 4.7 | 8.3 | — |
| send-saturation | 1024 | `zlink-framework-kotlin` | 5.668 | 2482.409 | 2655.419 | 2675.103 | 13.1 | 2.62 | 1132.7 | 13.8 | 847.2 | 2464 |
| send-saturation | 1024 | `zlink-kotlin` | 372.464 | 0.076 | 0.232 | 0.291 | 10.1 | 2.03 | 1047.7 | 4.2 | 1036.0 | 311 |
| send-saturation | 4096 | `grpc-c` | 61.411 | 0.000 | 0.000 | 0.000 | 9.5 | n/a | 111.1 | 26.3 | 106.7 | — |
| send-saturation | 4096 | `grpc-kotlin` | 33.752 | 0.126 | 0.177 | 0.206 | 13.7 | 2.74 | 1573.5 | 6.5 | 1256.9 | 333 |
| send-saturation | 4096 | `zlink-c` | 485.766 | 0.000 | 0.000 | 0.000 | 8.7 | n/a | 36.6 | 6.0 | 8.3 | — |
| send-saturation | 4096 | `zlink-framework-kotlin` | 8.671 | 819.060 | 897.205 | 918.998 | 13.3 | 2.66 | 1614.5 | 13.4 | 907.2 | 1051 |
| send-saturation | 4096 | `zlink-kotlin` | 256.755 | 0.084 | 0.233 | 0.285 | 10.4 | 2.07 | 1576.0 | 4.4 | 1041.2 | 375 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | client cores | saturation metric | reading | declared ceiling | saturated | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 1024 | `grpc-kotlin` | 1 | 100 | 0 | 1.0 | — | no | 0.60 | jvm_thread_cores | 0.072 | 1 | no | — |
| request-serial | 1024 | `zlink-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 1024 | `zlink-framework-kotlin` | 1 | 100 | 0 | 1.0 | — | no | 0.35 | jvm_thread_cores | 0.039 | 1 | no | — |
| request-serial | 1024 | `zlink-kotlin` | 1 | 100 | 0 | 1.0 | — | no | 0.52 | jvm_thread_cores | 0.082 | 1 | no | — |
| request-serial | 4096 | `grpc-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 4096 | `grpc-kotlin` | 1 | 100 | 0 | 1.0 | — | no | 0.61 | jvm_thread_cores | 0.072 | 1 | no | — |
| request-serial | 4096 | `zlink-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-serial | 4096 | `zlink-framework-kotlin` | 1 | 100 | 0 | 1.0 | — | no | 0.34 | jvm_thread_cores | 0.040 | 1 | no | — |
| request-serial | 4096 | `zlink-kotlin` | 1 | 100 | 0 | 1.0 | — | no | 0.54 | jvm_thread_cores | 0.088 | 1 | no | — |
| request-window | 1024 | `grpc-c` | 100 | — | — | 98.9 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 1024 | `grpc-kotlin` | 100 | 100 | 0 | 92.0 | — | no | 5.63 | jvm_thread_cores | 0.540 | 1 | no | — |
| request-window | 1024 | `zlink-c` | 100 | — | — | 86.9 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 1024 | `zlink-framework-kotlin` | 10 | 100 | 0 | 4.5 | — | no | 1.07 | jvm_thread_cores | 0.180 | 1 | no | — |
| request-window | 1024 | `zlink-kotlin` | 100 | 100 | 100 | n/a | — | no | 0.13 | jvm_thread_cores | 0.006 | 1 | no | — |
| request-window | 4096 | `grpc-c` | 100 | — | — | 98.8 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 4096 | `grpc-kotlin` | 100 | 100 | 0 | 93.1 | — | no | 4.87 | jvm_thread_cores | 0.487 | 1 | no | — |
| request-window | 4096 | `zlink-c` | 100 | — | — | 87.3 | — | no | n/a | client_cores | n/a | — | not judged | — |
| request-window | 4096 | `zlink-framework-kotlin` | 12 (low 10) | 100 | 0 | 4.5 | — | no | 1.11 | jvm_thread_cores | 0.186 | 1 | no | — |
| request-window | 4096 | `zlink-kotlin` | 100 | 100 | 100 | n/a | — | no | 0.13 | jvm_thread_cores | 0.006 | 1 | no | — |
| send-saturation | 1024 | `grpc-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 1024 | `grpc-kotlin` | 8 | 8 | 0 | 4.2 | 310 | no | 2.75 | jvm_thread_cores | 0.578 | 8 | no | server |
| send-saturation | 1024 | `zlink-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 1024 | `zlink-framework-kotlin` | 8 | 8 | 0 | 14071.3 | 2464 | no | 2.62 | jvm_thread_cores | 0.559 | 8 | no | server |
| send-saturation | 1024 | `zlink-kotlin` | 8 | 8 | 0 | 28.4 | 311 | no | 2.03 | jvm_thread_cores | 1.261 | 8 | no | server |
| send-saturation | 4096 | `grpc-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 4096 | `grpc-kotlin` | 8 | 8 | 0 | 4.3 | 333 | no | 2.74 | jvm_thread_cores | 0.575 | 8 | no | server |
| send-saturation | 4096 | `zlink-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | **client (G3 fail)** |
| send-saturation | 4096 | `zlink-framework-kotlin` | 8 | 8 | 0 | 7101.9 | 1051 | no | 2.66 | jvm_thread_cores | 0.600 | 8 | no | server |
| send-saturation | 4096 | `zlink-kotlin` | 8 | 8 | 0 | 21.6 | 375 | no | 2.07 | jvm_thread_cores | 1.273 | 8 | no | server |

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 3 | 4.4% | pass |
| request-serial | 1024 | `grpc-kotlin` | 3 | 1.5% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 37.9% | **fail** |
| request-serial | 1024 | `zlink-framework-kotlin` | 3 | 4.6% | pass |
| request-serial | 1024 | `zlink-kotlin` | 3 | 2.9% | pass |
| request-serial | 4096 | `grpc-c` | 3 | 0.5% | pass |
| request-serial | 4096 | `grpc-kotlin` | 3 | 4.7% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 12.6% | **fail** |
| request-serial | 4096 | `zlink-framework-kotlin` | 3 | 2.9% | pass |
| request-serial | 4096 | `zlink-kotlin` | 3 | 1.3% | pass |
| request-window | 1024 | `grpc-c` | 3 | 1.0% | pass |
| request-window | 1024 | `grpc-kotlin` | 3 | 0.5% | pass |
| request-window | 1024 | `zlink-c` | 3 | 7.4% | pass |
| request-window | 1024 | `zlink-framework-kotlin` | 3 | 1.7% | pass |
| request-window | 1024 | `zlink-kotlin` | 3 | n/a (3 run) | n/a |
| request-window | 4096 | `grpc-c` | 3 | 2.6% | pass |
| request-window | 4096 | `grpc-kotlin` | 3 | 2.9% | pass |
| request-window | 4096 | `zlink-c` | 3 | 28.7% | **fail** |
| request-window | 4096 | `zlink-framework-kotlin` | 3 | 0.6% | pass |
| request-window | 4096 | `zlink-kotlin` | 3 | n/a (3 run) | n/a |
| send-saturation | 1024 | `grpc-c` | 3 | 3.3% | pass |
| send-saturation | 1024 | `grpc-kotlin` | 3 | 2.4% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 2.9% | pass |
| send-saturation | 1024 | `zlink-framework-kotlin` | 3 | 12.8% | **fail** |
| send-saturation | 1024 | `zlink-kotlin` | 3 | 4.0% | pass |
| send-saturation | 4096 | `grpc-c` | 3 | 1.1% | pass |
| send-saturation | 4096 | `grpc-kotlin` | 3 | 2.4% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 1.0% | pass |
| send-saturation | 4096 | `zlink-framework-kotlin` | 3 | 4.0% | pass |
| send-saturation | 4096 | `zlink-kotlin` | 3 | 3.3% | pass |

## Contaminated cells (FB-008, excluded from tables and judgement)

None. Every cell drained within the bound.

## Judgement (spec 7.2, FB-005, FB-011)

| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-kotlin / zlink-c` | 1024 | n/a | **unsupported** | — | numerator zlink-kotlin-request-window@1024 has 3 run(s); G5 needs 3 |
| `zlink-kotlin / zlink-c` | 4096 | n/a | **unsupported** | — | numerator zlink-kotlin-request-window@4096 has 3 run(s); G5 needs 3; denominator zlink-c-request-window@4096 fails G5 at 28.7% (limit 10%) |
| `zlink-framework-kotlin / zlink-kotlin` | 1024 | n/a | **unsupported** | — | denominator zlink-kotlin-request-window@1024 has 3 run(s); G5 needs 3 |
| `zlink-framework-kotlin / zlink-kotlin` | 4096 | n/a | **unsupported** | — | denominator zlink-kotlin-request-window@4096 has 3 run(s); G5 needs 3 |

**kotlin: incomplete** — 4 of 4 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- kotlin-router-1: read structured with-grpc-cell-v1
- kotlin-router-2: read structured with-grpc-cell-v1
- kotlin-router-3: read structured with-grpc-cell-v1
- c-router-1: throughput read as KOPS (scale 1000)
- c-router-1: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- c-router-1: no client parallelism ceiling declared; spec 5.1 saturation not judged
- c-router-2: throughput read as KOPS (scale 1000)
- c-router-2: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- c-router-2: no client parallelism ceiling declared; spec 5.1 saturation not judged
- c-router-3: throughput read as KOPS (scale 1000)
- c-router-3: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- c-router-3: no client parallelism ceiling declared; spec 5.1 saturation not judged
