# S2 Java + Kotlin 보조 3-run 집계 (2026-09-09, 조용한 창; C 기준선 s1q)

원본: `framework/bench/grpc/log/java/with_grpc_java_{s2r_run1,s2s_run2,s2r_run3}/`, `with_grpc_kotlin_s2q_run{1,2,3}/`. 조건: Core 0.17.5, binding 0.17.6, JDK 22.0.2, warmup 20초, 5초 active. Java source 수정(FB-055, warmup abandoned 기록) 뒤 rebuild한 binary.

## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       13.63 KOPS |   13.96 MB/s |     0.071 ms |     0.104 ms |     0.157 ms |       2.0% |    14.8 MB |       2.4% |    13.8 MB |
      | grpc-java               | 1024B    |        6.00 KOPS |    6.15 MB/s |     0.166 ms |     0.224 ms |     0.302 ms |       1.9% |   318.5 MB |       1.9% |   933.7 MB |
      | zlink-c                 | 1024B    |        8.00 KOPS |    8.19 MB/s |     0.124 ms |     0.151 ms |     0.225 ms |       2.3% |     7.0 MB |       1.3% |     6.6 MB |
      | zlink-framework-java    | 1024B    |        0.47 KOPS |    0.48 MB/s |     2.137 ms |     2.579 ms |     2.765 ms |       1.9% |   355.0 MB |       2.3% |   396.5 MB |
      | zlink-java              | 1024B    |        6.61 KOPS |    6.76 MB/s |     0.151 ms |     0.202 ms |     0.283 ms |       2.3% |   755.9 MB |       1.3% |   537.2 MB |
      | grpc-c                  | 4096B    |       12.90 KOPS |   52.83 MB/s |     0.074 ms |     0.110 ms |     0.156 ms |       2.1% |  1146.5 MB |       2.4% |  1746.3 MB |
      | grpc-java               | 4096B    |        6.00 KOPS |   24.56 MB/s |     0.167 ms |     0.219 ms |     0.302 ms |       2.0% |   320.6 MB |       2.2% |  1067.7 MB |
      | zlink-c                 | 4096B    |        8.00 KOPS |   32.76 MB/s |     0.124 ms |     0.152 ms |     0.225 ms |       2.2% |    34.6 MB |       1.4% |    11.4 MB |
      | zlink-framework-java    | 4096B    |        0.47 KOPS |    1.90 MB/s |     2.150 ms |     2.564 ms |     2.676 ms |       2.0% |   369.1 MB |       2.1% |   361.6 MB |
      | zlink-java              | 4096B    |        6.65 KOPS |   27.22 MB/s |     0.150 ms |     0.198 ms |     0.264 ms |       2.3% |  1053.1 MB |       1.3% |  1028.2 MB |

  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       62.58 KOPS |   64.08 MB/s |     1.579 ms |     1.802 ms |     2.038 ms |       8.9% |    18.1 MB |      26.0% |    23.0 MB |
      | grpc-java               | 1024B    |      111.44 KOPS |  114.11 MB/s |     0.915 ms |     1.131 ms |     1.305 ms |      14.3% |   822.4 MB |      13.9% |  1176.3 MB |
      | grpc-kotlin             | 1024B    |       87.30 KOPS |   89.40 MB/s |     1.062 ms |     1.418 ms |     2.248 ms |      25.9% |   825.4 MB |      12.4% |  1172.2 MB |
      | zlink-c                 | 1024B    |      463.00 KOPS |  474.11 MB/s |     0.177 ms |     0.283 ms |     0.357 ms |       9.7% |     9.6 MB |       8.8% |     6.9 MB |
      | zlink-framework-java    | 1024B    |        2.34 KOPS |    2.39 MB/s |     1.973 ms |     2.844 ms |     3.102 ms |       5.5% |   371.8 MB |       5.2% |   369.9 MB |
      | zlink-framework-kotlin  | 1024B    |        2.26 KOPS |    2.32 MB/s |     2.013 ms |     2.892 ms |     3.142 ms |       5.6% |   372.9 MB |       5.0% |   362.7 MB |
      | zlink-java              | 1024B    |        0.00 KOPS |    0.00 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       0.1% |   184.1 MB |       0.1% |   123.6 MB |
      | grpc-c                  | 4096B    |       55.88 KOPS |  228.87 MB/s |     1.763 ms |     2.026 ms |     2.253 ms |       8.9% |  1146.5 MB |      26.5% |  1746.8 MB |
      | grpc-java               | 4096B    |       91.38 KOPS |  374.30 MB/s |     1.105 ms |     1.577 ms |     2.047 ms |      13.5% |   867.6 MB |      12.9% |  1155.9 MB |
      | zlink-c                 | 4096B    |      382.50 KOPS | 1566.74 MB/s |     0.207 ms |     0.347 ms |     0.432 ms |      11.8% |    37.0 MB |      10.5% |    11.5 MB |
      | zlink-framework-java    | 4096B    |        2.28 KOPS |    9.33 MB/s |     2.013 ms |     2.899 ms |     3.199 ms |       5.7% |   360.3 MB |       5.3% |   805.4 MB |
      | zlink-java              | 4096B    |        0.00 KOPS |    0.00 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       0.1% |   188.8 MB |       0.1% |   115.2 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       29.80 KOPS |   30.51 MB/s |  2497.207 ms |  3412.125 ms |  3439.610 ms |       6.4% |  1514.6 MB |      16.8% |  1746.6 MB |
      | grpc-java               | 1024B    |      244.26 KOPS |  250.12 MB/s | 18675.537 ms | 18806.847 ms | 18819.763 ms |      18.0% | 24248.1 MB |      72.6% | 19437.7 MB |
      | zlink-c                 | 1024B    |      554.47 KOPS |  567.77 MB/s |     0.882 ms |     1.599 ms |     1.968 ms |      10.5% |    11.5 MB |       9.7% |     9.5 MB |
      | zlink-framework-java    | 1024B    |        2.27 KOPS |    2.33 MB/s |     1.985 ms |     2.855 ms |     3.113 ms |       5.3% |   360.0 MB |       5.1% |   372.0 MB |
      | zlink-java              | 1024B    |        0.00 KOPS |    0.00 MB/s | 22044.028 ms | 22044.028 ms | 22044.028 ms |       5.1% |  2867.8 MB |       0.7% |   243.3 MB |
      | grpc-c                  | 4096B    |       31.15 KOPS |  127.58 MB/s |  2182.147 ms |  3897.705 ms |  3968.772 ms |       7.3% |  1862.4 MB |      17.5% |  2138.3 MB |
      | grpc-java               | 4096B    |      129.42 KOPS |  530.11 MB/s |  2187.882 ms |  3707.333 ms |  4041.575 ms |      15.9% | 19299.2 MB |      27.1% | 12158.0 MB |
      | zlink-c                 | 4096B    |      428.11 KOPS | 1753.56 MB/s |     0.358 ms |     0.634 ms |     0.826 ms |      12.4% |    39.0 MB |      11.3% |    10.9 MB |
      | zlink-framework-java    | 4096B    |        2.32 KOPS |    9.50 MB/s |     1.985 ms |     2.850 ms |     3.098 ms |       5.6% |   360.1 MB |       5.3% |   821.7 MB |
      | zlink-java              | 4096B    |        0.00 KOPS |    0.00 MB/s | 29420.284 ms | 29420.284 ms | 29420.284 ms |       5.1% |   574.0 MB |       0.1% |   143.8 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |     57.55 KMSG/s |   58.93 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.4% |  1146.5 MB |      23.8% |  1746.3 MB |
      | grpc-java               | 1024B    |     40.84 KMSG/s |   41.82 MB/s |     0.106 ms |     0.156 ms |     0.191 ms |       9.7% |   426.0 MB |       7.2% |  1092.0 MB |
      | zlink-c                 | 1024B    |    689.19 KMSG/s |  705.73 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.0% |    34.3 MB |       4.3% |    11.4 MB |
      | zlink-framework-java    | 1024B    |     10.52 KMSG/s |   10.77 MB/s |  2433.846 ms |  2593.367 ms |  2621.482 ms |      13.4% |   932.4 MB |      14.4% |   799.8 MB |
      | zlink-java              | 1024B    |    544.59 KMSG/s |  557.66 MB/s |     0.074 ms |     0.202 ms |     0.264 ms |       9.9% |  1085.5 MB |       4.6% |  1035.2 MB |
      | grpc-c                  | 4096B    |     49.36 KMSG/s |  202.16 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.1% |  1537.3 MB |      20.3% |  2054.4 MB |
      | grpc-java               | 4096B    |     41.46 KMSG/s |  169.82 MB/s |     0.104 ms |     0.151 ms |     0.180 ms |       9.3% |   414.4 MB |       7.2% |  1091.7 MB |
      | zlink-c                 | 4096B    |    481.24 KMSG/s | 1971.17 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.7% |    36.3 MB |       5.8% |    11.5 MB |
      | zlink-framework-java    | 4096B    |      9.87 KMSG/s |   40.42 MB/s |   832.710 ms |   905.903 ms |   927.347 ms |      13.0% |   897.4 MB |      13.5% |   785.0 MB |
      | zlink-java              | 4096B    |    348.09 KMSG/s | 1425.78 MB/s |     0.074 ms |     0.221 ms |     0.272 ms |      10.3% |  1076.0 MB |       5.1% |  1036.1 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-c-request-serial,local,1024,throughput,13634.000
RESULT,current,grpc-c-request-serial,local,1024,bandwidth,13.962
RESULT,current,grpc-c-request-serial,local,1024,latency,0.071
RESULT,current,grpc-c-request-serial,local,1024,latency_p95,0.104
RESULT,current,grpc-c-request-serial,local,1024,latency_p99,0.157
RESULT,current,grpc-c-request-serial,local,1024,client_cpu_percent,1.994
RESULT,current,grpc-c-request-serial,local,1024,client_memory_mb,14.797
RESULT,current,grpc-c-request-serial,local,1024,server_cpu_percent,2.430
RESULT,current,grpc-c-request-serial,local,1024,server_memory_mb,13.750
RESULT,current,grpc-java-request-serial,local,1024,throughput,6003.400
RESULT,current,grpc-java-request-serial,local,1024,bandwidth,6.147
RESULT,current,grpc-java-request-serial,local,1024,latency,0.166
RESULT,current,grpc-java-request-serial,local,1024,latency_p95,0.224
RESULT,current,grpc-java-request-serial,local,1024,latency_p99,0.302
RESULT,current,grpc-java-request-serial,local,1024,client_cpu_percent,1.950
RESULT,current,grpc-java-request-serial,local,1024,client_memory_mb,318.488
RESULT,current,grpc-java-request-serial,local,1024,server_cpu_percent,1.910
RESULT,current,grpc-java-request-serial,local,1024,server_memory_mb,933.652
RESULT,current,zlink-c-request-serial,local,1024,throughput,8002.000
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,8.194
RESULT,current,zlink-c-request-serial,local,1024,latency,0.124
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.151
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.225
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,2.255
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,6.965
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,1.340
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,6.562
RESULT,current,zlink-framework-java-request-serial,local,1024,throughput,467.800
RESULT,current,zlink-framework-java-request-serial,local,1024,bandwidth,0.479
RESULT,current,zlink-framework-java-request-serial,local,1024,latency,2.137
RESULT,current,zlink-framework-java-request-serial,local,1024,latency_p95,2.579
RESULT,current,zlink-framework-java-request-serial,local,1024,latency_p99,2.765
RESULT,current,zlink-framework-java-request-serial,local,1024,client_cpu_percent,1.920
RESULT,current,zlink-framework-java-request-serial,local,1024,client_memory_mb,355.023
RESULT,current,zlink-framework-java-request-serial,local,1024,server_cpu_percent,2.270
RESULT,current,zlink-framework-java-request-serial,local,1024,server_memory_mb,396.461
RESULT,current,zlink-java-request-serial,local,1024,throughput,6605.800
RESULT,current,zlink-java-request-serial,local,1024,bandwidth,6.764
RESULT,current,zlink-java-request-serial,local,1024,latency,0.151
RESULT,current,zlink-java-request-serial,local,1024,latency_p95,0.202
RESULT,current,zlink-java-request-serial,local,1024,latency_p99,0.283
RESULT,current,zlink-java-request-serial,local,1024,client_cpu_percent,2.270
RESULT,current,zlink-java-request-serial,local,1024,client_memory_mb,755.891
RESULT,current,zlink-java-request-serial,local,1024,server_cpu_percent,1.330
RESULT,current,zlink-java-request-serial,local,1024,server_memory_mb,537.207
RESULT,current,grpc-c-request-serial,local,4096,throughput,12898.000
RESULT,current,grpc-c-request-serial,local,4096,bandwidth,52.832
RESULT,current,grpc-c-request-serial,local,4096,latency,0.074
RESULT,current,grpc-c-request-serial,local,4096,latency_p95,0.110
RESULT,current,grpc-c-request-serial,local,4096,latency_p99,0.156
RESULT,current,grpc-c-request-serial,local,4096,client_cpu_percent,2.062
RESULT,current,grpc-c-request-serial,local,4096,client_memory_mb,1146.527
RESULT,current,grpc-c-request-serial,local,4096,server_cpu_percent,2.410
RESULT,current,grpc-c-request-serial,local,4096,server_memory_mb,1746.285
RESULT,current,grpc-java-request-serial,local,4096,throughput,5997.200
RESULT,current,grpc-java-request-serial,local,4096,bandwidth,24.565
RESULT,current,grpc-java-request-serial,local,4096,latency,0.167
RESULT,current,grpc-java-request-serial,local,4096,latency_p95,0.219
RESULT,current,grpc-java-request-serial,local,4096,latency_p99,0.302
RESULT,current,grpc-java-request-serial,local,4096,client_cpu_percent,2.040
RESULT,current,grpc-java-request-serial,local,4096,client_memory_mb,320.605
RESULT,current,grpc-java-request-serial,local,4096,server_cpu_percent,2.180
RESULT,current,grpc-java-request-serial,local,4096,server_memory_mb,1067.676
RESULT,current,zlink-c-request-serial,local,4096,throughput,7998.000
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,32.758
RESULT,current,zlink-c-request-serial,local,4096,latency,0.124
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.152
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.225
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,2.224
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,34.617
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,1.410
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,11.426
RESULT,current,zlink-framework-java-request-serial,local,4096,throughput,465.000
RESULT,current,zlink-framework-java-request-serial,local,4096,bandwidth,1.905
RESULT,current,zlink-framework-java-request-serial,local,4096,latency,2.150
RESULT,current,zlink-framework-java-request-serial,local,4096,latency_p95,2.564
RESULT,current,zlink-framework-java-request-serial,local,4096,latency_p99,2.676
RESULT,current,zlink-framework-java-request-serial,local,4096,client_cpu_percent,1.960
RESULT,current,zlink-framework-java-request-serial,local,4096,client_memory_mb,369.055
RESULT,current,zlink-framework-java-request-serial,local,4096,server_cpu_percent,2.060
RESULT,current,zlink-framework-java-request-serial,local,4096,server_memory_mb,361.637
RESULT,current,zlink-java-request-serial,local,4096,throughput,6645.400
RESULT,current,zlink-java-request-serial,local,4096,bandwidth,27.220
RESULT,current,zlink-java-request-serial,local,4096,latency,0.150
RESULT,current,zlink-java-request-serial,local,4096,latency_p95,0.198
RESULT,current,zlink-java-request-serial,local,4096,latency_p99,0.264
RESULT,current,zlink-java-request-serial,local,4096,client_cpu_percent,2.300
RESULT,current,zlink-java-request-serial,local,4096,client_memory_mb,1053.125
RESULT,current,zlink-java-request-serial,local,4096,server_cpu_percent,1.320
RESULT,current,zlink-java-request-serial,local,4096,server_memory_mb,1028.238
RESULT,current,grpc-c-request-window,local,1024,throughput,62583.000
RESULT,current,grpc-c-request-window,local,1024,bandwidth,64.085
RESULT,current,grpc-c-request-window,local,1024,latency,1.579
RESULT,current,grpc-c-request-window,local,1024,latency_p95,1.802
RESULT,current,grpc-c-request-window,local,1024,latency_p99,2.038
RESULT,current,grpc-c-request-window,local,1024,client_cpu_percent,8.877
RESULT,current,grpc-c-request-window,local,1024,client_memory_mb,18.062
RESULT,current,grpc-c-request-window,local,1024,server_cpu_percent,25.974
RESULT,current,grpc-c-request-window,local,1024,server_memory_mb,22.957
RESULT,current,grpc-java-request-window,local,1024,throughput,111439.600
RESULT,current,grpc-java-request-window,local,1024,bandwidth,114.114
RESULT,current,grpc-java-request-window,local,1024,latency,0.915
RESULT,current,grpc-java-request-window,local,1024,latency_p95,1.131
RESULT,current,grpc-java-request-window,local,1024,latency_p99,1.305
RESULT,current,grpc-java-request-window,local,1024,client_cpu_percent,14.327
RESULT,current,grpc-java-request-window,local,1024,client_memory_mb,822.449
RESULT,current,grpc-java-request-window,local,1024,server_cpu_percent,13.930
RESULT,current,grpc-java-request-window,local,1024,server_memory_mb,1176.312
RESULT,current,grpc-kotlin-request-window,local,1024,throughput,87300.200
RESULT,current,grpc-kotlin-request-window,local,1024,bandwidth,89.395
RESULT,current,grpc-kotlin-request-window,local,1024,latency,1.062
RESULT,current,grpc-kotlin-request-window,local,1024,latency_p95,1.418
RESULT,current,grpc-kotlin-request-window,local,1024,latency_p99,2.248
RESULT,current,grpc-kotlin-request-window,local,1024,client_cpu_percent,25.943
RESULT,current,grpc-kotlin-request-window,local,1024,client_memory_mb,825.391
RESULT,current,grpc-kotlin-request-window,local,1024,server_cpu_percent,12.430
RESULT,current,grpc-kotlin-request-window,local,1024,server_memory_mb,1172.160
RESULT,current,zlink-c-request-window,local,1024,throughput,462997.000
RESULT,current,zlink-c-request-window,local,1024,bandwidth,474.108
RESULT,current,zlink-c-request-window,local,1024,latency,0.177
RESULT,current,zlink-c-request-window,local,1024,latency_p95,0.283
RESULT,current,zlink-c-request-window,local,1024,latency_p99,0.357
RESULT,current,zlink-c-request-window,local,1024,client_cpu_percent,9.722
RESULT,current,zlink-c-request-window,local,1024,client_memory_mb,9.617
RESULT,current,zlink-c-request-window,local,1024,server_cpu_percent,8.770
RESULT,current,zlink-c-request-window,local,1024,server_memory_mb,6.914
RESULT,current,zlink-framework-java-request-window,local,1024,throughput,2336.400
RESULT,current,zlink-framework-java-request-window,local,1024,bandwidth,2.392
RESULT,current,zlink-framework-java-request-window,local,1024,latency,1.973
RESULT,current,zlink-framework-java-request-window,local,1024,latency_p95,2.844
RESULT,current,zlink-framework-java-request-window,local,1024,latency_p99,3.102
RESULT,current,zlink-framework-java-request-window,local,1024,client_cpu_percent,5.517
RESULT,current,zlink-framework-java-request-window,local,1024,client_memory_mb,371.832
RESULT,current,zlink-framework-java-request-window,local,1024,server_cpu_percent,5.180
RESULT,current,zlink-framework-java-request-window,local,1024,server_memory_mb,369.859
RESULT,current,zlink-framework-kotlin-request-window,local,1024,throughput,2262.600
RESULT,current,zlink-framework-kotlin-request-window,local,1024,bandwidth,2.317
RESULT,current,zlink-framework-kotlin-request-window,local,1024,latency,2.013
RESULT,current,zlink-framework-kotlin-request-window,local,1024,latency_p95,2.892
RESULT,current,zlink-framework-kotlin-request-window,local,1024,latency_p99,3.142
RESULT,current,zlink-framework-kotlin-request-window,local,1024,client_cpu_percent,5.577
RESULT,current,zlink-framework-kotlin-request-window,local,1024,client_memory_mb,372.855
RESULT,current,zlink-framework-kotlin-request-window,local,1024,server_cpu_percent,5.010
RESULT,current,zlink-framework-kotlin-request-window,local,1024,server_memory_mb,362.664
RESULT,current,zlink-java-request-window,local,1024,throughput,0.000
RESULT,current,zlink-java-request-window,local,1024,bandwidth,0.000
RESULT,current,zlink-java-request-window,local,1024,latency,0.000
RESULT,current,zlink-java-request-window,local,1024,latency_p95,0.000
RESULT,current,zlink-java-request-window,local,1024,latency_p99,0.000
RESULT,current,zlink-java-request-window,local,1024,client_cpu_percent,0.103
RESULT,current,zlink-java-request-window,local,1024,client_memory_mb,184.129
RESULT,current,zlink-java-request-window,local,1024,server_cpu_percent,0.080
RESULT,current,zlink-java-request-window,local,1024,server_memory_mb,123.578
RESULT,current,grpc-c-request-window,local,4096,throughput,55876.000
RESULT,current,grpc-c-request-window,local,4096,bandwidth,228.870
RESULT,current,grpc-c-request-window,local,4096,latency,1.763
RESULT,current,grpc-c-request-window,local,4096,latency_p95,2.026
RESULT,current,grpc-c-request-window,local,4096,latency_p99,2.253
RESULT,current,grpc-c-request-window,local,4096,client_cpu_percent,8.915
RESULT,current,grpc-c-request-window,local,4096,client_memory_mb,1146.527
RESULT,current,grpc-c-request-window,local,4096,server_cpu_percent,26.529
RESULT,current,grpc-c-request-window,local,4096,server_memory_mb,1746.832
RESULT,current,grpc-java-request-window,local,4096,throughput,91382.200
RESULT,current,grpc-java-request-window,local,4096,bandwidth,374.301
RESULT,current,grpc-java-request-window,local,4096,latency,1.105
RESULT,current,grpc-java-request-window,local,4096,latency_p95,1.577
RESULT,current,grpc-java-request-window,local,4096,latency_p99,2.047
RESULT,current,grpc-java-request-window,local,4096,client_cpu_percent,13.517
RESULT,current,grpc-java-request-window,local,4096,client_memory_mb,867.645
RESULT,current,grpc-java-request-window,local,4096,server_cpu_percent,12.870
RESULT,current,grpc-java-request-window,local,4096,server_memory_mb,1155.910
RESULT,current,zlink-c-request-window,local,4096,throughput,382505.000
RESULT,current,zlink-c-request-window,local,4096,bandwidth,1566.742
RESULT,current,zlink-c-request-window,local,4096,latency,0.207
RESULT,current,zlink-c-request-window,local,4096,latency_p95,0.347
RESULT,current,zlink-c-request-window,local,4096,latency_p99,0.432
RESULT,current,zlink-c-request-window,local,4096,client_cpu_percent,11.818
RESULT,current,zlink-c-request-window,local,4096,client_memory_mb,36.961
RESULT,current,zlink-c-request-window,local,4096,server_cpu_percent,10.540
RESULT,current,zlink-c-request-window,local,4096,server_memory_mb,11.516
RESULT,current,zlink-framework-java-request-window,local,4096,throughput,2276.800
RESULT,current,zlink-framework-java-request-window,local,4096,bandwidth,9.326
RESULT,current,zlink-framework-java-request-window,local,4096,latency,2.013
RESULT,current,zlink-framework-java-request-window,local,4096,latency_p95,2.899
RESULT,current,zlink-framework-java-request-window,local,4096,latency_p99,3.199
RESULT,current,zlink-framework-java-request-window,local,4096,client_cpu_percent,5.697
RESULT,current,zlink-framework-java-request-window,local,4096,client_memory_mb,360.273
RESULT,current,zlink-framework-java-request-window,local,4096,server_cpu_percent,5.260
RESULT,current,zlink-framework-java-request-window,local,4096,server_memory_mb,805.363
RESULT,current,zlink-java-request-window,local,4096,throughput,0.000
RESULT,current,zlink-java-request-window,local,4096,bandwidth,0.000
RESULT,current,zlink-java-request-window,local,4096,latency,0.000
RESULT,current,zlink-java-request-window,local,4096,latency_p95,0.000
RESULT,current,zlink-java-request-window,local,4096,latency_p99,0.000
RESULT,current,zlink-java-request-window,local,4096,client_cpu_percent,0.103
RESULT,current,zlink-java-request-window,local,4096,client_memory_mb,188.797
RESULT,current,zlink-java-request-window,local,4096,server_cpu_percent,0.080
RESULT,current,zlink-java-request-window,local,4096,server_memory_mb,115.246
RESULT,current,grpc-c-request-backpressure,local,1024,throughput,29799.000
RESULT,current,grpc-c-request-backpressure,local,1024,bandwidth,30.514
RESULT,current,grpc-c-request-backpressure,local,1024,latency,2497.207
RESULT,current,grpc-c-request-backpressure,local,1024,latency_p95,3412.125
RESULT,current,grpc-c-request-backpressure,local,1024,latency_p99,3439.610
RESULT,current,grpc-c-request-backpressure,local,1024,client_cpu_percent,6.361
RESULT,current,grpc-c-request-backpressure,local,1024,client_memory_mb,1514.633
RESULT,current,grpc-c-request-backpressure,local,1024,server_cpu_percent,16.844
RESULT,current,grpc-c-request-backpressure,local,1024,server_memory_mb,1746.621
RESULT,current,grpc-java-request-backpressure,local,1024,throughput,244259.400
RESULT,current,grpc-java-request-backpressure,local,1024,bandwidth,250.122
RESULT,current,grpc-java-request-backpressure,local,1024,latency,18675.537
RESULT,current,grpc-java-request-backpressure,local,1024,latency_p95,18806.847
RESULT,current,grpc-java-request-backpressure,local,1024,latency_p99,18819.763
RESULT,current,grpc-java-request-backpressure,local,1024,client_cpu_percent,17.993
RESULT,current,grpc-java-request-backpressure,local,1024,client_memory_mb,24248.133
RESULT,current,grpc-java-request-backpressure,local,1024,server_cpu_percent,72.600
RESULT,current,grpc-java-request-backpressure,local,1024,server_memory_mb,19437.672
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,554467.000
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,567.774
RESULT,current,zlink-c-request-backpressure,local,1024,latency,0.882
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,1.599
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,1.968
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,10.480
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,11.492
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,9.650
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,9.465
RESULT,current,zlink-framework-java-request-backpressure,local,1024,throughput,2273.000
RESULT,current,zlink-framework-java-request-backpressure,local,1024,bandwidth,2.328
RESULT,current,zlink-framework-java-request-backpressure,local,1024,latency,1.985
RESULT,current,zlink-framework-java-request-backpressure,local,1024,latency_p95,2.855
RESULT,current,zlink-framework-java-request-backpressure,local,1024,latency_p99,3.113
RESULT,current,zlink-framework-java-request-backpressure,local,1024,client_cpu_percent,5.278
RESULT,current,zlink-framework-java-request-backpressure,local,1024,client_memory_mb,359.988
RESULT,current,zlink-framework-java-request-backpressure,local,1024,server_cpu_percent,5.130
RESULT,current,zlink-framework-java-request-backpressure,local,1024,server_memory_mb,371.953
RESULT,current,zlink-java-request-backpressure,local,1024,throughput,0.200
RESULT,current,zlink-java-request-backpressure,local,1024,bandwidth,0.000
RESULT,current,zlink-java-request-backpressure,local,1024,latency,22044.028
RESULT,current,zlink-java-request-backpressure,local,1024,latency_p95,22044.028
RESULT,current,zlink-java-request-backpressure,local,1024,latency_p99,22044.028
RESULT,current,zlink-java-request-backpressure,local,1024,client_cpu_percent,5.125
RESULT,current,zlink-java-request-backpressure,local,1024,client_memory_mb,2867.840
RESULT,current,zlink-java-request-backpressure,local,1024,server_cpu_percent,0.740
RESULT,current,zlink-java-request-backpressure,local,1024,server_memory_mb,243.301
RESULT,current,grpc-c-request-backpressure,local,4096,throughput,31148.000
RESULT,current,grpc-c-request-backpressure,local,4096,bandwidth,127.582
RESULT,current,grpc-c-request-backpressure,local,4096,latency,2182.147
RESULT,current,grpc-c-request-backpressure,local,4096,latency_p95,3897.705
RESULT,current,grpc-c-request-backpressure,local,4096,latency_p99,3968.772
RESULT,current,grpc-c-request-backpressure,local,4096,client_cpu_percent,7.280
RESULT,current,grpc-c-request-backpressure,local,4096,client_memory_mb,1862.371
RESULT,current,grpc-c-request-backpressure,local,4096,server_cpu_percent,17.479
RESULT,current,grpc-c-request-backpressure,local,4096,server_memory_mb,2138.344
RESULT,current,grpc-java-request-backpressure,local,4096,throughput,129420.800
RESULT,current,grpc-java-request-backpressure,local,4096,bandwidth,530.108
RESULT,current,grpc-java-request-backpressure,local,4096,latency,2187.882
RESULT,current,grpc-java-request-backpressure,local,4096,latency_p95,3707.333
RESULT,current,grpc-java-request-backpressure,local,4096,latency_p99,4041.575
RESULT,current,grpc-java-request-backpressure,local,4096,client_cpu_percent,15.939
RESULT,current,grpc-java-request-backpressure,local,4096,client_memory_mb,19299.176
RESULT,current,grpc-java-request-backpressure,local,4096,server_cpu_percent,27.060
RESULT,current,grpc-java-request-backpressure,local,4096,server_memory_mb,12157.980
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,428114.000
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,1753.557
RESULT,current,zlink-c-request-backpressure,local,4096,latency,0.358
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,0.634
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,0.826
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,12.400
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,39.004
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,11.319
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,10.883
RESULT,current,zlink-framework-java-request-backpressure,local,4096,throughput,2318.400
RESULT,current,zlink-framework-java-request-backpressure,local,4096,bandwidth,9.496
RESULT,current,zlink-framework-java-request-backpressure,local,4096,latency,1.985
RESULT,current,zlink-framework-java-request-backpressure,local,4096,latency_p95,2.850
RESULT,current,zlink-framework-java-request-backpressure,local,4096,latency_p99,3.098
RESULT,current,zlink-framework-java-request-backpressure,local,4096,client_cpu_percent,5.578
RESULT,current,zlink-framework-java-request-backpressure,local,4096,client_memory_mb,360.141
RESULT,current,zlink-framework-java-request-backpressure,local,4096,server_cpu_percent,5.260
RESULT,current,zlink-framework-java-request-backpressure,local,4096,server_memory_mb,821.730
RESULT,current,zlink-java-request-backpressure,local,4096,throughput,0.200
RESULT,current,zlink-java-request-backpressure,local,4096,bandwidth,0.001
RESULT,current,zlink-java-request-backpressure,local,4096,latency,29420.284
RESULT,current,zlink-java-request-backpressure,local,4096,latency_p95,29420.284
RESULT,current,zlink-java-request-backpressure,local,4096,latency_p99,29420.284
RESULT,current,zlink-java-request-backpressure,local,4096,client_cpu_percent,5.117
RESULT,current,zlink-java-request-backpressure,local,4096,client_memory_mb,573.988
RESULT,current,zlink-java-request-backpressure,local,4096,server_cpu_percent,0.090
RESULT,current,zlink-java-request-backpressure,local,4096,server_memory_mb,143.828
RESULT,current,grpc-c-send-saturation,local,1024,throughput,57548.000
RESULT,current,grpc-c-send-saturation,local,1024,bandwidth,58.929
RESULT,current,grpc-c-send-saturation,local,1024,latency,0.000
RESULT,current,grpc-c-send-saturation,local,1024,latency_p95,0.000
RESULT,current,grpc-c-send-saturation,local,1024,latency_p99,0.000
RESULT,current,grpc-c-send-saturation,local,1024,client_cpu_percent,9.434
RESULT,current,grpc-c-send-saturation,local,1024,client_memory_mb,1146.527
RESULT,current,grpc-c-send-saturation,local,1024,server_cpu_percent,23.833
RESULT,current,grpc-c-send-saturation,local,1024,server_memory_mb,1746.285
RESULT,current,grpc-java-send-saturation,local,1024,throughput,40838.400
RESULT,current,grpc-java-send-saturation,local,1024,bandwidth,41.819
RESULT,current,grpc-java-send-saturation,local,1024,latency,0.106
RESULT,current,grpc-java-send-saturation,local,1024,latency_p95,0.156
RESULT,current,grpc-java-send-saturation,local,1024,latency_p99,0.191
RESULT,current,grpc-java-send-saturation,local,1024,client_cpu_percent,9.749
RESULT,current,grpc-java-send-saturation,local,1024,client_memory_mb,425.969
RESULT,current,grpc-java-send-saturation,local,1024,server_cpu_percent,7.190
RESULT,current,grpc-java-send-saturation,local,1024,server_memory_mb,1092.035
RESULT,current,zlink-c-send-saturation,local,1024,throughput,689189.000
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,705.729
RESULT,current,zlink-c-send-saturation,local,1024,latency,0.000
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,0.000
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,0.000
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,8.005
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,34.305
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,4.270
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,11.426
RESULT,current,zlink-framework-java-send-saturation,local,1024,throughput,10522.000
RESULT,current,zlink-framework-java-send-saturation,local,1024,bandwidth,10.775
RESULT,current,zlink-framework-java-send-saturation,local,1024,latency,2433.846
RESULT,current,zlink-framework-java-send-saturation,local,1024,latency_p95,2593.367
RESULT,current,zlink-framework-java-send-saturation,local,1024,latency_p99,2621.482
RESULT,current,zlink-framework-java-send-saturation,local,1024,client_cpu_percent,13.369
RESULT,current,zlink-framework-java-send-saturation,local,1024,client_memory_mb,932.406
RESULT,current,zlink-framework-java-send-saturation,local,1024,server_cpu_percent,14.450
RESULT,current,zlink-framework-java-send-saturation,local,1024,server_memory_mb,799.781
RESULT,current,zlink-java-send-saturation,local,1024,throughput,544588.400
RESULT,current,zlink-java-send-saturation,local,1024,bandwidth,557.659
RESULT,current,zlink-java-send-saturation,local,1024,latency,0.074
RESULT,current,zlink-java-send-saturation,local,1024,latency_p95,0.202
RESULT,current,zlink-java-send-saturation,local,1024,latency_p99,0.264
RESULT,current,zlink-java-send-saturation,local,1024,client_cpu_percent,9.869
RESULT,current,zlink-java-send-saturation,local,1024,client_memory_mb,1085.488
RESULT,current,zlink-java-send-saturation,local,1024,server_cpu_percent,4.610
RESULT,current,zlink-java-send-saturation,local,1024,server_memory_mb,1035.160
RESULT,current,grpc-c-send-saturation,local,4096,throughput,49356.000
RESULT,current,grpc-c-send-saturation,local,4096,bandwidth,202.161
RESULT,current,grpc-c-send-saturation,local,4096,latency,0.000
RESULT,current,grpc-c-send-saturation,local,4096,latency_p95,0.000
RESULT,current,grpc-c-send-saturation,local,4096,latency_p99,0.000
RESULT,current,grpc-c-send-saturation,local,4096,client_cpu_percent,9.058
RESULT,current,grpc-c-send-saturation,local,4096,client_memory_mb,1537.324
RESULT,current,grpc-c-send-saturation,local,4096,server_cpu_percent,20.345
RESULT,current,grpc-c-send-saturation,local,4096,server_memory_mb,2054.367
RESULT,current,grpc-java-send-saturation,local,4096,throughput,41459.000
RESULT,current,grpc-java-send-saturation,local,4096,bandwidth,169.816
RESULT,current,grpc-java-send-saturation,local,4096,latency,0.104
RESULT,current,grpc-java-send-saturation,local,4096,latency_p95,0.151
RESULT,current,grpc-java-send-saturation,local,4096,latency_p99,0.180
RESULT,current,grpc-java-send-saturation,local,4096,client_cpu_percent,9.270
RESULT,current,grpc-java-send-saturation,local,4096,client_memory_mb,414.379
RESULT,current,grpc-java-send-saturation,local,4096,server_cpu_percent,7.210
RESULT,current,grpc-java-send-saturation,local,4096,server_memory_mb,1091.684
RESULT,current,zlink-c-send-saturation,local,4096,throughput,481242.000
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,1971.166
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.000
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,0.000
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,0.000
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,8.724
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,36.297
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,5.820
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,11.508
RESULT,current,zlink-framework-java-send-saturation,local,4096,throughput,9867.600
RESULT,current,zlink-framework-java-send-saturation,local,4096,bandwidth,40.418
RESULT,current,zlink-framework-java-send-saturation,local,4096,latency,832.710
RESULT,current,zlink-framework-java-send-saturation,local,4096,latency_p95,905.903
RESULT,current,zlink-framework-java-send-saturation,local,4096,latency_p99,927.347
RESULT,current,zlink-framework-java-send-saturation,local,4096,client_cpu_percent,13.024
RESULT,current,zlink-framework-java-send-saturation,local,4096,client_memory_mb,897.410
RESULT,current,zlink-framework-java-send-saturation,local,4096,server_cpu_percent,13.500
RESULT,current,zlink-framework-java-send-saturation,local,4096,server_memory_mb,785.023
RESULT,current,zlink-java-send-saturation,local,4096,throughput,348089.600
RESULT,current,zlink-java-send-saturation,local,4096,bandwidth,1425.775
RESULT,current,zlink-java-send-saturation,local,4096,latency,0.074
RESULT,current,zlink-java-send-saturation,local,4096,latency_p95,0.221
RESULT,current,zlink-java-send-saturation,local,4096,latency_p99,0.272
RESULT,current,zlink-java-send-saturation,local,4096,client_cpu_percent,10.269
RESULT,current,zlink-java-send-saturation,local,4096,client_memory_mb,1076.035
RESULT,current,zlink-java-send-saturation,local,4096,server_cpu_percent,5.120
RESULT,current,zlink-java-send-saturation,local,4096,server_memory_mb,1036.070


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 13.634 | 0.071 | 0.104 | 0.157 | 2.0 | n/a | 14.8 | 2.4 | 13.8 | — |
| request-serial | 1024 | `grpc-java` | 6.003 | 0.166 | 0.224 | 0.302 | 1.9 | 0.39 | 318.5 | 1.9 | 933.7 | 298 |
| request-serial | 1024 | `zlink-c` | 8.002 | 0.124 | 0.151 | 0.225 | 2.3 | n/a | 7.0 | 1.3 | 6.6 | — |
| request-serial | 1024 | `zlink-framework-java` | 0.468 | 2.137 | 2.579 | 2.765 | 1.9 | 0.38 | 355.0 | 2.3 | 396.5 | 271 |
| request-serial | 1024 | `zlink-java` | 6.606 | 0.151 | 0.202 | 0.283 | 2.3 | 0.45 | 755.9 | 1.3 | 537.2 | 304 |
| request-serial | 4096 | `grpc-c` | 12.898 | 0.074 | 0.110 | 0.156 | 2.1 | n/a | 1146.5 | 2.4 | 1746.3 | — |
| request-serial | 4096 | `grpc-java` | 5.997 | 0.167 | 0.219 | 0.302 | 2.0 | 0.41 | 320.6 | 2.2 | 1067.7 | 299 |
| request-serial | 4096 | `zlink-c` | 7.998 | 0.124 | 0.152 | 0.225 | 2.2 | n/a | 34.6 | 1.4 | 11.4 | — |
| request-serial | 4096 | `zlink-framework-java` | 0.465 | 2.150 | 2.564 | 2.676 | 2.0 | 0.39 | 369.1 | 2.1 | 361.6 | 271 |
| request-serial | 4096 | `zlink-java` | 6.645 | 0.150 | 0.198 | 0.264 | 2.3 | 0.46 | 1053.1 | 1.3 | 1028.2 | 300 |
| request-window | 1024 | `grpc-c` | 62.583 | 1.579 | 1.802 | 2.038 | 8.9 | n/a | 18.1 | 26.0 | 23.0 | — |
| request-window | 1024 | `grpc-java` | 111.440 | 0.915 | 1.131 | 1.305 | 14.3 | 2.87 | 822.4 | 13.9 | 1176.3 | 452 |
| request-window | 1024 | `grpc-kotlin` | 87.300 | 1.062 | 1.418 | 2.248 | 25.9 | 5.19 | 825.4 | 12.4 | 1172.2 | 472 |
| request-window | 1024 | `zlink-c` | 462.997 | 0.177 | 0.283 | 0.357 | 9.7 | n/a | 9.6 | 8.8 | 6.9 | — |
| request-window | 1024 | `zlink-framework-java` | 2.336 | 1.973 | 2.844 | 3.102 | 5.5 | 1.10 | 371.8 | 5.2 | 369.9 | 275 |
| request-window | 1024 | `zlink-framework-kotlin` | 2.263 | 2.013 | 2.892 | 3.142 | 5.6 | 1.12 | 372.9 | 5.0 | 362.7 | 275 |
| request-window | 1024 | `zlink-java` | 0.000 | 0.000 | 0.000 | 0.000 | 0.1 | 0.02 | 184.1 | 0.1 | 123.6 | 272 |
| request-window | 4096 | `grpc-c` | 55.876 | 1.763 | 2.026 | 2.253 | 8.9 | n/a | 1146.5 | 26.5 | 1746.8 | — |
| request-window | 4096 | `grpc-java` | 91.382 | 1.105 | 1.577 | 2.047 | 13.5 | 2.70 | 867.6 | 12.9 | 1155.9 | 486 |
| request-window | 4096 | `zlink-c` | 382.505 | 0.207 | 0.347 | 0.432 | 11.8 | n/a | 37.0 | 10.5 | 11.5 | — |
| request-window | 4096 | `zlink-framework-java` | 2.277 | 2.013 | 2.899 | 3.199 | 5.7 | 1.14 | 360.3 | 5.3 | 805.4 | 276 |
| request-window | 4096 | `zlink-java` | 0.000 | 0.000 | 0.000 | 0.000 | 0.1 | 0.02 | 188.8 | 0.1 | 115.2 | 273 |
| request-backpressure | 1024 | `grpc-c` | 29.799 | 2497.207 | 3412.125 | 3439.610 | 6.4 | n/a | 1514.6 | 16.8 | 1746.6 | — |
| request-backpressure | 1024 | `grpc-java` | 244.259 | 18675.537 | 18806.847 | 18819.763 | 18.0 | 3.60 | 24248.1 | 72.6 | 19437.7 | 338 |
| request-backpressure | 1024 | `zlink-c` | 554.467 | 0.882 | 1.599 | 1.968 | 10.5 | n/a | 11.5 | 9.7 | 9.5 | — |
| request-backpressure | 1024 | `zlink-framework-java` | 2.273 | 1.985 | 2.855 | 3.113 | 5.3 | 1.06 | 360.0 | 5.1 | 372.0 | 275 |
| request-backpressure | 1024 | `zlink-java` | 0.000 | 22044.028 | 22044.028 | 22044.028 | 5.1 | 1.03 | 2867.8 | 0.7 | 243.3 | 278 |
| request-backpressure | 4096 | `grpc-c` | 31.148 | 2182.147 | 3897.705 | 3968.772 | 7.3 | n/a | 1862.4 | 17.5 | 2138.3 | — |
| request-backpressure | 4096 | `grpc-java` | 129.421 | 2187.882 | 3707.333 | 4041.575 | 15.9 | 3.19 | 19299.2 | 27.1 | 12158.0 | 350 |
| request-backpressure | 4096 | `zlink-c` | 428.114 | 0.358 | 0.634 | 0.826 | 12.4 | n/a | 39.0 | 11.3 | 10.9 | — |
| request-backpressure | 4096 | `zlink-framework-java` | 2.318 | 1.985 | 2.850 | 3.098 | 5.6 | 1.12 | 360.1 | 5.3 | 821.7 | 277 |
| request-backpressure | 4096 | `zlink-java` | 0.000 | 29420.284 | 29420.284 | 29420.284 | 5.1 | 1.02 | 574.0 | 0.1 | 143.8 | 275 |
| send-saturation | 1024 | `grpc-c` | 57.548 | 0.000 | 0.000 | 0.000 | 9.4 | n/a | 1146.5 | 23.8 | 1746.3 | — |
| send-saturation | 1024 | `grpc-java` | 40.838 | 0.106 | 0.156 | 0.191 | 9.7 | 1.95 | 426.0 | 7.2 | 1092.0 | 375 |
| send-saturation | 1024 | `zlink-c` | 689.189 | 0.000 | 0.000 | 0.000 | 8.0 | n/a | 34.3 | 4.3 | 11.4 | — |
| send-saturation | 1024 | `zlink-framework-java` | 10.522 | 2433.846 | 2593.367 | 2621.482 | 13.4 | 2.67 | 932.4 | 14.4 | 799.8 | 2318 |
| send-saturation | 1024 | `zlink-java` | 544.588 | 0.074 | 0.202 | 0.264 | 9.9 | 1.97 | 1085.5 | 4.6 | 1035.2 | 429 |
| send-saturation | 4096 | `grpc-c` | 49.356 | 0.000 | 0.000 | 0.000 | 9.1 | n/a | 1537.3 | 20.3 | 2054.4 | — |
| send-saturation | 4096 | `grpc-java` | 41.459 | 0.104 | 0.151 | 0.180 | 9.3 | 1.85 | 414.4 | 7.2 | 1091.7 | 398 |
| send-saturation | 4096 | `zlink-c` | 481.242 | 0.000 | 0.000 | 0.000 | 8.7 | n/a | 36.3 | 5.8 | 11.5 | — |
| send-saturation | 4096 | `zlink-framework-java` | 9.868 | 832.710 | 905.903 | 927.347 | 13.0 | 2.60 | 897.4 | 13.5 | 785.0 | 920 |
| send-saturation | 4096 | `zlink-java` | 348.090 | 0.074 | 0.221 | 0.272 | 10.3 | 2.05 | 1076.0 | 5.1 | 1036.1 | 446 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | saturated | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 1024 | `grpc-java` | 1 | — | 0 | 1.0 | 298 | no | 0.39 | jvm_thread_cores | 0.075 | 1 | no | 30017 | 0 | — |
| request-serial | 1024 | `zlink-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 1024 | `zlink-framework-java` | 1 | — | 0 | 1.0 | 271 | no | 0.38 | jvm_thread_cores | 0.040 | 1 | no | 2339 | 0 | — |
| request-serial | 1024 | `zlink-java` | 1 | — | 0 | 1.0 | 304 | no | 0.45 | jvm_thread_cores | 0.087 | 1 | no | 33029 | 0 | — |
| request-serial | 4096 | `grpc-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 4096 | `grpc-java` | 1 | — | 0 | 1.0 | 299 | no | 0.41 | jvm_thread_cores | 0.081 | 1 | no | 29986 | 0 | — |
| request-serial | 4096 | `zlink-c` | 1 | — | — | 1.0 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 4096 | `zlink-framework-java` | 1 | — | 0 | 1.0 | 271 | no | 0.39 | jvm_thread_cores | 0.040 | 1 | no | 2325 | 0 | — |
| request-serial | 4096 | `zlink-java` | 1 | — | 0 | 1.0 | 300 | no | 0.46 | jvm_thread_cores | 0.084 | 1 | no | 33227 | 0 | — |
| request-window | 1024 | `grpc-c` | 100 | — | — | 98.8 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-window | 1024 | `grpc-java` | 100 | 100 | 0 | 101.9 | 452 | no | 2.87 | jvm_thread_cores | 0.286 | 1 | no | 557198 | 0 | — |
| request-window | 1024 | `grpc-kotlin` | 100 | 100 | 0 | 92.7 | 472 | no | 5.19 | jvm_thread_cores | 0.530 | 1 | no | 436501 | 0 | — |
| request-window | 1024 | `zlink-c` | 100 | — | — | 82.0 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-window | 1024 | `zlink-framework-java` | 10 | 100 | 0 | 4.6 | 275 | no | 1.10 | jvm_thread_cores | 0.181 | 1 | no | 11682 | 0 | — |
| request-window | 1024 | `zlink-framework-kotlin` | 10 | 100 | 0 | 4.6 | 275 | no | 1.12 | jvm_thread_cores | 0.182 | 1 | no | 11313 | 0 | — |
| request-window | 1024 | `zlink-java` | 100 | 100 | 100 | n/a | 272 | no | 0.02 | jvm_thread_cores | 0.001 | 1 | no | 100 | 0 | — |
| request-window | 4096 | `grpc-c` | 100 | — | — | 98.5 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-window | 4096 | `grpc-java` | 100 | 100 | 0 | 101.0 | 486 | no | 2.70 | jvm_thread_cores | 0.321 | 1 | no | 456911 | 0 | — |
| request-window | 4096 | `zlink-c` | 100 | — | — | 79.2 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-window | 4096 | `zlink-framework-java` | 12 (low 11) | 100 | 0 | 4.6 | 276 | no | 1.14 | jvm_thread_cores | 0.184 | 1 | no | 11384 | 0 | — |
| request-window | 4096 | `zlink-java` | 100 | 100 | 100 | n/a | 273 | no | 0.02 | jvm_thread_cores | 0.001 | 1 | no | 100 | 0 | — |
| request-backpressure | 1024 | `grpc-c` | 121009 (low 106335) | — | — | 74414.3 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-backpressure | 1024 | `grpc-java` | 1268725 (low 1136161) | — | 0 | 4561675.4 | 338 | no | 3.60 | jvm_thread_cores | 0.165 | 1 | no | 1221297 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 10890 (low 1201) | — | — | 489.0 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-backpressure | 1024 | `zlink-framework-java` | 11 (low 9) | — | 0 | 4.5 | 275 | no | 1.06 | jvm_thread_cores | 0.179 | 1 | no | 11365 | 1 | — |
| request-backpressure | 1024 | `zlink-java` | 1 | — | 0 | 4.4 | 278 | no | 1.03 | jvm_thread_cores | 0.000 | 1 | no | 1 | 0 | — |
| request-backpressure | 4096 | `grpc-c` | 113354 (low 89855) | — | — | 67969.5 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-backpressure | 4096 | `grpc-java` | 540732 (low 439538) | — | 0 | 283157.4 | 350 | no | 3.19 | jvm_thread_cores | 0.441 | 1 | no | 647104 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 406 | — | — | 153.3 | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-backpressure | 4096 | `zlink-framework-java` | 13 (low 10) | — | 0 | 4.6 | 277 | no | 1.12 | jvm_thread_cores | 0.186 | 1 | no | 11592 | 0 | — |
| request-backpressure | 4096 | `zlink-java` | 1 | — | 0 | 5.9 | 275 | no | 1.02 | jvm_thread_cores | 0.000 | 1 | no | 1 | 0 | — |
| send-saturation | 1024 | `grpc-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | **client (G3 fail)** |
| send-saturation | 1024 | `grpc-java` | 8 | — | 0 | 4.3 | 375 | no | 1.95 | jvm_thread_cores | 0.448 | 8 | no | 204192 | 0 | server |
| send-saturation | 1024 | `zlink-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | n/a | 2541587 | **client (G3 fail)** |
| send-saturation | 1024 | `zlink-framework-java` | 8 | — | 0 | 25608.9 | 2318 | no | 2.67 | jvm_thread_cores | 0.537 | 8 | no | 52610 | 0 | server |
| send-saturation | 1024 | `zlink-java` | 8 | — | 0 | 40.3 | 429 | no | 1.97 | jvm_thread_cores | 1.247 | 8 | no | 2722942 | 0 | server |
| send-saturation | 4096 | `grpc-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | **client (G3 fail)** |
| send-saturation | 4096 | `grpc-java` | 8 | — | 0 | 4.3 | 398 | no | 1.85 | jvm_thread_cores | 0.472 | 8 | no | 207295 | 0 | server |
| send-saturation | 4096 | `zlink-c` | — | — | — | n/a | — | no | n/a | client_cores | n/a | — | not judged | n/a | 4202845 | **client (G3 fail)** |
| send-saturation | 4096 | `zlink-framework-java` | 8 | — | 0 | 8216.8 | 920 | no | 2.60 | jvm_thread_cores | 0.542 | 8 | no | 49338 | 0 | server |
| send-saturation | 4096 | `zlink-java` | 8 | — | 0 | 25.7 | 446 | no | 2.05 | jvm_thread_cores | 1.270 | 8 | no | 1740448 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | n/a | n/a | n/a |
| request-serial | 1024 | `grpc-java` | 1 | 1 | http://127.0.0.1:5240/bench/start |
| request-serial | 1024 | `zlink-c` | n/a | n/a | n/a |
| request-serial | 1024 | `zlink-framework-java` | 1 | 1 | http://127.0.0.1:5252/bench/start |
| request-serial | 1024 | `zlink-java` | 1 | 1 | http://127.0.0.1:5245/bench/start |
| request-serial | 4096 | `grpc-c` | n/a | n/a | n/a |
| request-serial | 4096 | `grpc-java` | 1 | 1 | http://127.0.0.1:5240/bench/start |
| request-serial | 4096 | `zlink-c` | n/a | n/a | n/a |
| request-serial | 4096 | `zlink-framework-java` | 1 | 1 | http://127.0.0.1:5252/bench/start |
| request-serial | 4096 | `zlink-java` | 1 | 1 | http://127.0.0.1:5245/bench/start |
| request-window | 1024 | `grpc-c` | n/a | n/a | n/a |
| request-window | 1024 | `grpc-java` | 1 | 100 | http://127.0.0.1:5240/bench/start |
| request-window | 1024 | `grpc-kotlin` | 1 | 100 | http://127.0.0.1:5260/bench/start |
| request-window | 1024 | `zlink-c` | n/a | n/a | n/a |
| request-window | 1024 | `zlink-framework-java` | 1 | 100 | http://127.0.0.1:5252/bench/start |
| request-window | 1024 | `zlink-framework-kotlin` | 1 | 100 | http://127.0.0.1:5272/bench/start |
| request-window | 1024 | `zlink-java` | 1 | 100 | http://127.0.0.1:5245/bench/start |
| request-window | 4096 | `grpc-c` | n/a | n/a | n/a |
| request-window | 4096 | `grpc-java` | 1 | 100 | http://127.0.0.1:5240/bench/start |
| request-window | 4096 | `zlink-c` | n/a | n/a | n/a |
| request-window | 4096 | `zlink-framework-java` | 1 | 100 | http://127.0.0.1:5252/bench/start |
| request-window | 4096 | `zlink-java` | 1 | 100 | http://127.0.0.1:5245/bench/start |
| request-backpressure | 1024 | `grpc-c` | n/a | n/a | n/a |
| request-backpressure | 1024 | `grpc-java` | 1 | unbounded | http://127.0.0.1:5240/bench/start |
| request-backpressure | 1024 | `zlink-c` | n/a | n/a | n/a |
| request-backpressure | 1024 | `zlink-framework-java` | 1 | unbounded | http://127.0.0.1:5252/bench/start |
| request-backpressure | 1024 | `zlink-java` | 1 | unbounded | http://127.0.0.1:5245/bench/start |
| request-backpressure | 4096 | `grpc-c` | n/a | n/a | n/a |
| request-backpressure | 4096 | `grpc-java` | 1 | unbounded | http://127.0.0.1:5240/bench/start |
| request-backpressure | 4096 | `zlink-c` | n/a | n/a | n/a |
| request-backpressure | 4096 | `zlink-framework-java` | 1 | unbounded | http://127.0.0.1:5252/bench/start |
| request-backpressure | 4096 | `zlink-java` | 1 | unbounded | http://127.0.0.1:5245/bench/start |
| send-saturation | 1024 | `grpc-c` | n/a | n/a | n/a |
| send-saturation | 1024 | `grpc-java` | 8 | 1 | http://127.0.0.1:5240/bench/start |
| send-saturation | 1024 | `zlink-c` | n/a | n/a | n/a |
| send-saturation | 1024 | `zlink-framework-java` | 8 | 1 | http://127.0.0.1:5252/bench/start |
| send-saturation | 1024 | `zlink-java` | 8 | 1 | http://127.0.0.1:5245/bench/start |
| send-saturation | 4096 | `grpc-c` | n/a | n/a | n/a |
| send-saturation | 4096 | `grpc-java` | 8 | 1 | http://127.0.0.1:5240/bench/start |
| send-saturation | 4096 | `zlink-c` | n/a | n/a | n/a |
| send-saturation | 4096 | `zlink-framework-java` | 8 | 1 | http://127.0.0.1:5252/bench/start |
| send-saturation | 4096 | `zlink-java` | 8 | 1 | http://127.0.0.1:5245/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 3 | 2.4% | pass |
| request-serial | 1024 | `grpc-java` | 3 | 2.3% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 10.6% | **fail** |
| request-serial | 1024 | `zlink-framework-java` | 3 | 2.7% | pass |
| request-serial | 1024 | `zlink-java` | 3 | 2.2% | pass |
| request-serial | 4096 | `grpc-c` | 3 | 0.5% | pass |
| request-serial | 4096 | `grpc-java` | 3 | 4.1% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 13.3% | **fail** |
| request-serial | 4096 | `zlink-framework-java` | 3 | 2.0% | pass |
| request-serial | 4096 | `zlink-java` | 3 | 1.9% | pass |
| request-window | 1024 | `grpc-c` | 3 | 3.2% | pass |
| request-window | 1024 | `grpc-java` | 3 | 2.1% | pass |
| request-window | 1024 | `grpc-kotlin` | 3 | 2.9% | pass |
| request-window | 1024 | `zlink-c` | 3 | 12.0% | **fail** |
| request-window | 1024 | `zlink-framework-java` | 3 | 3.0% | pass |
| request-window | 1024 | `zlink-framework-kotlin` | 3 | 3.5% | pass |
| request-window | 1024 | `zlink-java` | 3 | n/a (3 run) | n/a |
| request-window | 4096 | `grpc-c` | 3 | 2.0% | pass |
| request-window | 4096 | `grpc-java` | 3 | 4.6% | pass |
| request-window | 4096 | `zlink-c` | 3 | 11.8% | **fail** |
| request-window | 4096 | `zlink-framework-java` | 3 | 2.4% | pass |
| request-window | 4096 | `zlink-java` | 3 | n/a (3 run) | n/a |
| request-backpressure | 1024 | `grpc-c` | 3 | 2.5% | pass |
| request-backpressure | 1024 | `grpc-java` | 3 | 10.1% | **fail** |
| request-backpressure | 1024 | `zlink-c` | 3 | 4.3% | pass |
| request-backpressure | 1024 | `zlink-framework-java` | 3 | 3.8% | pass |
| request-backpressure | 1024 | `zlink-java` | 3 | 0.0% | pass |
| request-backpressure | 4096 | `grpc-c` | 3 | 13.7% | **fail** |
| request-backpressure | 4096 | `grpc-java` | 3 | 6.6% | pass |
| request-backpressure | 4096 | `zlink-c` | 3 | 3.0% | pass |
| request-backpressure | 4096 | `zlink-framework-java` | 3 | 0.8% | pass |
| request-backpressure | 4096 | `zlink-java` | 3 | 0.0% | pass |
| send-saturation | 1024 | `grpc-c` | 3 | 1.1% | pass |
| send-saturation | 1024 | `grpc-java` | 3 | 0.7% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 3.6% | pass |
| send-saturation | 1024 | `zlink-framework-java` | 3 | 16.5% | **fail** |
| send-saturation | 1024 | `zlink-java` | 3 | 3.2% | pass |
| send-saturation | 4096 | `grpc-c` | 3 | 5.7% | pass |
| send-saturation | 4096 | `grpc-java` | 3 | 2.6% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 4.3% | pass |
| send-saturation | 4096 | `zlink-framework-java` | 3 | 14.4% | **fail** |
| send-saturation | 4096 | `zlink-java` | 3 | 0.4% | pass |

## Contaminated cells (FB-008, excluded from tables and judgement)

None. Every cell drained within the bound.

## Incomplete server-driven cells (excluded from tables and judgement)

None. Every server-driven source has target stats.

## Judgement (spec 7.2) on `request-window`

| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-java / zlink-c` | 1024 | n/a | **unsupported** | — | numerator zlink-java-request-window@1024 has 3 run(s); G5 needs 3; denominator zlink-c-request-window@1024 fails G5 at 12.0% (limit 10%) |
| `zlink-java / zlink-c` | 4096 | n/a | **unsupported** | — | numerator zlink-java-request-window@4096 has 3 run(s); G5 needs 3; denominator zlink-c-request-window@4096 fails G5 at 11.8% (limit 10%) |
| `zlink-framework-java / zlink-java` | 1024 | n/a | **unsupported** | — | denominator zlink-java-request-window@1024 has 3 run(s); G5 needs 3 |
| `zlink-framework-java / zlink-java` | 4096 | n/a | **unsupported** | — | denominator zlink-java-request-window@4096 has 3 run(s); G5 needs 3 |

**java: incomplete** — 4 of 4 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- grpc-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-window-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-window-4096-run1: read structured cell data from 1 file(s)
- grpc-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- grpc-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-window-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-window-4096-run1: read structured cell data from 1 file(s)
- zlink-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- zlink-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-window-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-window-4096-run1: read structured cell data from 1 file(s)
- grpc-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- grpc-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-window-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-window-4096-run1: read structured cell data from 1 file(s)
- zlink-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- zlink-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- grpc-java-request-window-1024-run1: read structured cell data from 1 file(s)
- grpc-java-request-window-4096-run1: read structured cell data from 1 file(s)
- grpc-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- grpc-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-request-window-4096-run1: read structured cell data from 1 file(s)
- zlink-framework-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-backpressure-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-backpressure-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-serial-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-serial-4096-run1: read structured cell data from 1 file(s)
- zlink-java-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-java-request-window-4096-run1: read structured cell data from 1 file(s)
- zlink-java-send-saturation-1024-run1: read structured cell data from 1 file(s)
- zlink-java-send-saturation-4096-run1: read structured cell data from 1 file(s)
- grpc-kotlin-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-kotlin-request-window-1024-run1: read structured cell data from 1 file(s)
- grpc-kotlin-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-kotlin-request-window-1024-run1: read structured cell data from 1 file(s)
- grpc-kotlin-request-window-1024-run1: read structured cell data from 1 file(s)
- zlink-framework-kotlin-request-window-1024-run1: read structured cell data from 1 file(s)
- with_grpc_c_s1q_run1_20260909_180125: throughput read as KOPS (scale 1000)
- with_grpc_c_s1q_run1_20260909_180125: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- with_grpc_c_s1q_run1_20260909_180125: no client parallelism ceiling declared; spec 5.1 saturation not judged
- with_grpc_c_s1q_run2_20260909_180841: throughput read as KOPS (scale 1000)
- with_grpc_c_s1q_run2_20260909_180841: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- with_grpc_c_s1q_run2_20260909_180841: no client parallelism ceiling declared; spec 5.1 saturation not judged
- with_grpc_c_s1q_run3_20260909_181053: throughput read as KOPS (scale 1000)
- with_grpc_c_s1q_run3_20260909_181053: dropped 4 out-of-spec scenario(s): grpc-c-request-saturation, grpc-c-send-blocking, zlink-c-request-saturation, zlink-c-send-blocking
- with_grpc_c_s1q_run3_20260909_181053: no client parallelism ceiling declared; spec 5.1 saturation not judged
