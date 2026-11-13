## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-java               | 1024B    |        8.34 KOPS |    8.54 MB/s |     0.119 ms |     0.233 ms |     0.306 ms |      18.6% |   256.3 MB |      17.3% |   373.1 MB |
      | zlink-c                 | 1024B    |       15.17 KOPS |   15.54 MB/s |     0.060 ms |     0.072 ms |     0.086 ms |       4.4% |     9.8 MB |       3.3% |     8.1 MB |
      | zlink-framework-java    | 1024B    |        5.64 KOPS |    5.78 MB/s |     0.176 ms |     0.322 ms |     0.408 ms |      29.1% |   312.6 MB |      24.9% |   315.1 MB |
      | zlink-java              | 1024B    |       13.74 KOPS |   14.07 MB/s |     0.073 ms |     0.093 ms |     0.113 ms |       8.9% |   449.2 MB |       7.2% |   341.8 MB |
      | grpc-java               | 4096B    |        7.96 KOPS |   32.60 MB/s |     0.125 ms |     0.252 ms |     0.337 ms |      18.4% |   242.4 MB |      17.0% |   446.0 MB |
      | zlink-c                 | 4096B    |       14.33 KOPS |   58.68 MB/s |     0.063 ms |     0.077 ms |     0.096 ms |       4.3% |     9.6 MB |       3.3% |     8.3 MB |
      | zlink-framework-java    | 4096B    |        4.53 KOPS |   18.57 MB/s |     0.220 ms |     0.350 ms |     0.424 ms |      33.2% |   311.4 MB |      25.2% |   315.1 MB |
      | zlink-java              | 4096B    |       13.15 KOPS |   53.87 MB/s |     0.076 ms |     0.094 ms |     0.116 ms |       9.0% |   434.8 MB |       7.2% |   410.2 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      127.49 KOPS |  130.55 MB/s |   109.022 ms |   118.773 ms |   120.315 ms |      32.3% |    74.8 MB |       8.8% |    11.0 MB |
      | zlink-framework-java    | 1024B    |       37.72 KOPS |   38.62 MB/s |     3.737 ms |    21.743 ms |    30.582 ms |      61.4% |   400.9 MB |      43.9% |   512.7 MB |
      | zlink-java              | 1024B    |      145.33 KOPS |  148.82 MB/s |    91.433 ms |   114.958 ms |   132.139 ms |      80.4% |   841.2 MB |      28.6% |   667.7 MB |
      | zlink-c                 | 4096B    |       40.68 KOPS |  166.62 MB/s |     6.031 ms |     6.619 ms |     7.108 ms |      18.2% |    18.6 MB |       6.3% |     9.7 MB |
      | zlink-framework-java    | 4096B    |       35.65 KOPS |  146.00 MB/s |     3.208 ms |    13.681 ms |    22.992 ms |      58.9% |   428.0 MB |      52.1% |   567.9 MB |
      | zlink-java              | 4096B    |      104.77 KOPS |  429.13 MB/s |     3.939 ms |     6.355 ms |     8.518 ms |      56.4% |   870.6 MB |      32.6% |   981.9 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-java               | 1024B    |     32.92 KMSG/s |   33.71 MB/s |     0.122 ms |     0.235 ms |     0.340 ms |      35.0% |   308.6 MB |      30.0% |   461.4 MB |
      | zlink-c                 | 1024B    |    527.26 KMSG/s |  539.91 MB/s |    12.843 ms |     3.640 ms |     3.903 ms |      13.9% |    11.2 MB |       5.2% |    22.2 MB |
      | zlink-framework-java    | 1024B    |    106.35 KMSG/s |  108.90 MB/s |     0.226 ms |     0.245 ms |     4.451 ms |      27.8% |   333.6 MB |      35.8% |   653.2 MB |
      | zlink-java              | 1024B    |    553.83 KMSG/s |  567.13 MB/s |     8.641 ms |    27.578 ms |    29.018 ms |      64.8% |   900.9 MB |      23.4% |   823.5 MB |
      | grpc-java               | 4096B    |     31.93 KMSG/s |  130.80 MB/s |     0.126 ms |     0.226 ms |     0.348 ms |      34.7% |   424.4 MB |      28.6% |   446.1 MB |
      | zlink-c                 | 4096B    |    207.41 KMSG/s |  849.54 MB/s |     0.904 ms |     1.178 ms |     1.880 ms |       5.0% |    11.2 MB |       2.9% |    15.1 MB |
      | zlink-framework-java    | 4096B    |    102.63 KMSG/s |  420.37 MB/s |     0.402 ms |     2.356 ms |     5.311 ms |      29.4% |   415.0 MB |      36.0% |   753.2 MB |
      | zlink-java              | 4096B    |    203.85 KMSG/s |  834.98 MB/s |     3.344 ms |    19.713 ms |    23.271 ms |      50.4% |   951.7 MB |      23.6% |   808.3 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-java-request-serial,local,1024,throughput,8340.600
RESULT,current,grpc-java-request-serial,local,1024,bandwidth,8.541
RESULT,current,grpc-java-request-serial,local,1024,latency,0.119
RESULT,current,grpc-java-request-serial,local,1024,latency_p95,0.233
RESULT,current,grpc-java-request-serial,local,1024,latency_p99,0.306
RESULT,current,grpc-java-request-serial,local,1024,client_cpu_percent,18.575
RESULT,current,grpc-java-request-serial,local,1024,client_memory_mb,256.301
RESULT,current,grpc-java-request-serial,local,1024,server_cpu_percent,17.300
RESULT,current,grpc-java-request-serial,local,1024,server_memory_mb,373.113
RESULT,current,zlink-c-request-serial,local,1024,throughput,15171.400
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,15.536
RESULT,current,zlink-c-request-serial,local,1024,latency,0.060
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.072
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.086
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,4.397
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,9.809
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,3.312
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,8.105
RESULT,current,zlink-framework-java-request-serial,local,1024,throughput,5641.400
RESULT,current,zlink-framework-java-request-serial,local,1024,bandwidth,5.777
RESULT,current,zlink-framework-java-request-serial,local,1024,latency,0.176
RESULT,current,zlink-framework-java-request-serial,local,1024,latency_p95,0.322
RESULT,current,zlink-framework-java-request-serial,local,1024,latency_p99,0.408
RESULT,current,zlink-framework-java-request-serial,local,1024,client_cpu_percent,29.125
RESULT,current,zlink-framework-java-request-serial,local,1024,client_memory_mb,312.641
RESULT,current,zlink-framework-java-request-serial,local,1024,server_cpu_percent,24.850
RESULT,current,zlink-framework-java-request-serial,local,1024,server_memory_mb,315.062
RESULT,current,zlink-java-request-serial,local,1024,throughput,13736.800
RESULT,current,zlink-java-request-serial,local,1024,bandwidth,14.066
RESULT,current,zlink-java-request-serial,local,1024,latency,0.073
RESULT,current,zlink-java-request-serial,local,1024,latency_p95,0.093
RESULT,current,zlink-java-request-serial,local,1024,latency_p99,0.113
RESULT,current,zlink-java-request-serial,local,1024,client_cpu_percent,8.925
RESULT,current,zlink-java-request-serial,local,1024,client_memory_mb,449.152
RESULT,current,zlink-java-request-serial,local,1024,server_cpu_percent,7.250
RESULT,current,zlink-java-request-serial,local,1024,server_memory_mb,341.793
RESULT,current,grpc-java-request-serial,local,4096,throughput,7958.000
RESULT,current,grpc-java-request-serial,local,4096,bandwidth,32.596
RESULT,current,grpc-java-request-serial,local,4096,latency,0.125
RESULT,current,grpc-java-request-serial,local,4096,latency_p95,0.252
RESULT,current,grpc-java-request-serial,local,4096,latency_p99,0.337
RESULT,current,grpc-java-request-serial,local,4096,client_cpu_percent,18.450
RESULT,current,grpc-java-request-serial,local,4096,client_memory_mb,242.438
RESULT,current,grpc-java-request-serial,local,4096,server_cpu_percent,16.975
RESULT,current,grpc-java-request-serial,local,4096,server_memory_mb,446.035
RESULT,current,zlink-c-request-serial,local,4096,throughput,14326.200
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,58.680
RESULT,current,zlink-c-request-serial,local,4096,latency,0.063
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.077
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.096
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,4.284
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,9.598
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,3.349
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.270
RESULT,current,zlink-framework-java-request-serial,local,4096,throughput,4534.600
RESULT,current,zlink-framework-java-request-serial,local,4096,bandwidth,18.574
RESULT,current,zlink-framework-java-request-serial,local,4096,latency,0.220
RESULT,current,zlink-framework-java-request-serial,local,4096,latency_p95,0.350
RESULT,current,zlink-framework-java-request-serial,local,4096,latency_p99,0.424
RESULT,current,zlink-framework-java-request-serial,local,4096,client_cpu_percent,33.250
RESULT,current,zlink-framework-java-request-serial,local,4096,client_memory_mb,311.422
RESULT,current,zlink-framework-java-request-serial,local,4096,server_cpu_percent,25.250
RESULT,current,zlink-framework-java-request-serial,local,4096,server_memory_mb,315.066
RESULT,current,zlink-java-request-serial,local,4096,throughput,13151.000
RESULT,current,zlink-java-request-serial,local,4096,bandwidth,53.866
RESULT,current,zlink-java-request-serial,local,4096,latency,0.076
RESULT,current,zlink-java-request-serial,local,4096,latency_p95,0.094
RESULT,current,zlink-java-request-serial,local,4096,latency_p99,0.116
RESULT,current,zlink-java-request-serial,local,4096,client_cpu_percent,8.975
RESULT,current,zlink-java-request-serial,local,4096,client_memory_mb,434.809
RESULT,current,zlink-java-request-serial,local,4096,server_cpu_percent,7.225
RESULT,current,zlink-java-request-serial,local,4096,server_memory_mb,410.242
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,127491.800
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,130.552
RESULT,current,zlink-c-request-backpressure,local,1024,latency,109.022
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,118.773
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,120.315
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,32.333
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,74.762
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.823
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,10.953
RESULT,current,zlink-framework-java-request-backpressure,local,1024,throughput,37719.200
RESULT,current,zlink-framework-java-request-backpressure,local,1024,bandwidth,38.624
RESULT,current,zlink-framework-java-request-backpressure,local,1024,latency,3.737
RESULT,current,zlink-framework-java-request-backpressure,local,1024,latency_p95,21.743
RESULT,current,zlink-framework-java-request-backpressure,local,1024,latency_p99,30.582
RESULT,current,zlink-framework-java-request-backpressure,local,1024,client_cpu_percent,61.425
RESULT,current,zlink-framework-java-request-backpressure,local,1024,client_memory_mb,400.906
RESULT,current,zlink-framework-java-request-backpressure,local,1024,server_cpu_percent,43.900
RESULT,current,zlink-framework-java-request-backpressure,local,1024,server_memory_mb,512.703
RESULT,current,zlink-java-request-backpressure,local,1024,throughput,145331.200
RESULT,current,zlink-java-request-backpressure,local,1024,bandwidth,148.819
RESULT,current,zlink-java-request-backpressure,local,1024,latency,91.433
RESULT,current,zlink-java-request-backpressure,local,1024,latency_p95,114.958
RESULT,current,zlink-java-request-backpressure,local,1024,latency_p99,132.139
RESULT,current,zlink-java-request-backpressure,local,1024,client_cpu_percent,80.375
RESULT,current,zlink-java-request-backpressure,local,1024,client_memory_mb,841.211
RESULT,current,zlink-java-request-backpressure,local,1024,server_cpu_percent,28.600
RESULT,current,zlink-java-request-backpressure,local,1024,server_memory_mb,667.664
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,40679.100
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,166.622
RESULT,current,zlink-c-request-backpressure,local,4096,latency,6.031
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,6.619
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,7.108
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,18.192
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,18.641
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,6.274
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,9.686
RESULT,current,zlink-framework-java-request-backpressure,local,4096,throughput,35645.400
RESULT,current,zlink-framework-java-request-backpressure,local,4096,bandwidth,146.004
RESULT,current,zlink-framework-java-request-backpressure,local,4096,latency,3.208
RESULT,current,zlink-framework-java-request-backpressure,local,4096,latency_p95,13.681
RESULT,current,zlink-framework-java-request-backpressure,local,4096,latency_p99,22.992
RESULT,current,zlink-framework-java-request-backpressure,local,4096,client_cpu_percent,58.950
RESULT,current,zlink-framework-java-request-backpressure,local,4096,client_memory_mb,427.977
RESULT,current,zlink-framework-java-request-backpressure,local,4096,server_cpu_percent,52.150
RESULT,current,zlink-framework-java-request-backpressure,local,4096,server_memory_mb,567.938
RESULT,current,zlink-java-request-backpressure,local,4096,throughput,104767.200
RESULT,current,zlink-java-request-backpressure,local,4096,bandwidth,429.126
RESULT,current,zlink-java-request-backpressure,local,4096,latency,3.939
RESULT,current,zlink-java-request-backpressure,local,4096,latency_p95,6.355
RESULT,current,zlink-java-request-backpressure,local,4096,latency_p99,8.518
RESULT,current,zlink-java-request-backpressure,local,4096,client_cpu_percent,56.425
RESULT,current,zlink-java-request-backpressure,local,4096,client_memory_mb,870.562
RESULT,current,zlink-java-request-backpressure,local,4096,server_cpu_percent,32.575
RESULT,current,zlink-java-request-backpressure,local,4096,server_memory_mb,981.863
RESULT,current,grpc-java-send-saturation,local,1024,throughput,32915.200
RESULT,current,grpc-java-send-saturation,local,1024,bandwidth,33.705
RESULT,current,grpc-java-send-saturation,local,1024,latency,0.122
RESULT,current,grpc-java-send-saturation,local,1024,latency_p95,0.235
RESULT,current,grpc-java-send-saturation,local,1024,latency_p99,0.340
RESULT,current,grpc-java-send-saturation,local,1024,client_cpu_percent,35.050
RESULT,current,grpc-java-send-saturation,local,1024,client_memory_mb,308.555
RESULT,current,grpc-java-send-saturation,local,1024,server_cpu_percent,30.000
RESULT,current,grpc-java-send-saturation,local,1024,server_memory_mb,461.438
RESULT,current,zlink-c-send-saturation,local,1024,throughput,527255.600
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,539.910
RESULT,current,zlink-c-send-saturation,local,1024,latency,12.843
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,3.640
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,3.903
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,13.936
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,11.184
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,5.249
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,22.164
RESULT,current,zlink-framework-java-send-saturation,local,1024,throughput,106350.600
RESULT,current,zlink-framework-java-send-saturation,local,1024,bandwidth,108.903
RESULT,current,zlink-framework-java-send-saturation,local,1024,latency,0.226
RESULT,current,zlink-framework-java-send-saturation,local,1024,latency_p95,0.245
RESULT,current,zlink-framework-java-send-saturation,local,1024,latency_p99,4.451
RESULT,current,zlink-framework-java-send-saturation,local,1024,client_cpu_percent,27.825
RESULT,current,zlink-framework-java-send-saturation,local,1024,client_memory_mb,333.570
RESULT,current,zlink-framework-java-send-saturation,local,1024,server_cpu_percent,35.775
RESULT,current,zlink-framework-java-send-saturation,local,1024,server_memory_mb,653.191
RESULT,current,zlink-java-send-saturation,local,1024,throughput,553834.400
RESULT,current,zlink-java-send-saturation,local,1024,bandwidth,567.126
RESULT,current,zlink-java-send-saturation,local,1024,latency,8.641
RESULT,current,zlink-java-send-saturation,local,1024,latency_p95,27.578
RESULT,current,zlink-java-send-saturation,local,1024,latency_p99,29.018
RESULT,current,zlink-java-send-saturation,local,1024,client_cpu_percent,64.825
RESULT,current,zlink-java-send-saturation,local,1024,client_memory_mb,900.895
RESULT,current,zlink-java-send-saturation,local,1024,server_cpu_percent,23.425
RESULT,current,zlink-java-send-saturation,local,1024,server_memory_mb,823.523
RESULT,current,grpc-java-send-saturation,local,4096,throughput,31933.200
RESULT,current,grpc-java-send-saturation,local,4096,bandwidth,130.798
RESULT,current,grpc-java-send-saturation,local,4096,latency,0.126
RESULT,current,grpc-java-send-saturation,local,4096,latency_p95,0.226
RESULT,current,grpc-java-send-saturation,local,4096,latency_p99,0.348
RESULT,current,grpc-java-send-saturation,local,4096,client_cpu_percent,34.700
RESULT,current,grpc-java-send-saturation,local,4096,client_memory_mb,424.367
RESULT,current,grpc-java-send-saturation,local,4096,server_cpu_percent,28.600
RESULT,current,grpc-java-send-saturation,local,4096,server_memory_mb,446.078
RESULT,current,zlink-c-send-saturation,local,4096,throughput,207407.800
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,849.542
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.904
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,1.178
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,1.880
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,4.980
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,11.164
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,2.870
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,15.086
RESULT,current,zlink-framework-java-send-saturation,local,4096,throughput,102628.600
RESULT,current,zlink-framework-java-send-saturation,local,4096,bandwidth,420.367
RESULT,current,zlink-framework-java-send-saturation,local,4096,latency,0.402
RESULT,current,zlink-framework-java-send-saturation,local,4096,latency_p95,2.356
RESULT,current,zlink-framework-java-send-saturation,local,4096,latency_p99,5.311
RESULT,current,zlink-framework-java-send-saturation,local,4096,client_cpu_percent,29.450
RESULT,current,zlink-framework-java-send-saturation,local,4096,client_memory_mb,415.000
RESULT,current,zlink-framework-java-send-saturation,local,4096,server_cpu_percent,36.025
RESULT,current,zlink-framework-java-send-saturation,local,4096,server_memory_mb,753.238
RESULT,current,zlink-java-send-saturation,local,4096,throughput,203851.800
RESULT,current,zlink-java-send-saturation,local,4096,bandwidth,834.977
RESULT,current,zlink-java-send-saturation,local,4096,latency,3.344
RESULT,current,zlink-java-send-saturation,local,4096,latency_p95,19.713
RESULT,current,zlink-java-send-saturation,local,4096,latency_p99,23.271
RESULT,current,zlink-java-send-saturation,local,4096,client_cpu_percent,50.400
RESULT,current,zlink-java-send-saturation,local,4096,client_memory_mb,951.699
RESULT,current,zlink-java-send-saturation,local,4096,server_cpu_percent,23.600
RESULT,current,zlink-java-send-saturation,local,4096,server_memory_mb,808.348


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 8.341 | 0.119 | 0.233 | 0.306 | 18.6 | 1.49 | 256.3 | 17.3 | 373.1 | 349 |
| request-serial | 1024 | `zlink-c` | 15.171 | 0.060 | 0.072 | 0.086 | 4.4 | n/a | 9.8 | 3.3 | 8.1 | 0 |
| request-serial | 1024 | `zlink-framework-java` | 5.641 | 0.176 | 0.322 | 0.408 | 29.1 | 2.33 | 312.6 | 24.9 | 315.1 | 341 |
| request-serial | 1024 | `zlink-java` | 13.737 | 0.073 | 0.093 | 0.113 | 8.9 | 0.71 | 449.2 | 7.2 | 341.8 | 357 |
| request-serial | 4096 | `grpc-java` | 7.958 | 0.125 | 0.252 | 0.337 | 18.4 | 1.48 | 242.4 | 17.0 | 446.0 | 348 |
| request-serial | 4096 | `zlink-c` | 14.326 | 0.063 | 0.077 | 0.096 | 4.3 | n/a | 9.6 | 3.3 | 8.3 | 0 |
| request-serial | 4096 | `zlink-framework-java` | 4.535 | 0.220 | 0.350 | 0.424 | 33.2 | 2.66 | 311.4 | 25.2 | 315.1 | 350 |
| request-serial | 4096 | `zlink-java` | 13.151 | 0.076 | 0.094 | 0.116 | 9.0 | 0.72 | 434.8 | 7.2 | 410.2 | 353 |
| request-backpressure | 1024 | `zlink-c` | 127.492 | 109.022 | 118.773 | 120.315 | 32.3 | n/a | 74.8 | 8.8 | 11.0 | 21 |
| request-backpressure | 1024 | `zlink-framework-java` | 37.719 | 3.737 | 21.743 | 30.582 | 61.4 | 4.91 | 400.9 | 43.9 | 512.7 | 399 |
| request-backpressure | 1024 | `zlink-java` | 145.331 | 91.433 | 114.958 | 132.139 | 80.4 | 6.43 | 841.2 | 28.6 | 667.7 | 532 |
| request-backpressure | 4096 | `zlink-c` | 40.679 | 6.031 | 6.619 | 7.108 | 18.2 | n/a | 18.6 | 6.3 | 9.7 | 1 |
| request-backpressure | 4096 | `zlink-framework-java` | 35.645 | 3.208 | 13.681 | 22.992 | 58.9 | 4.72 | 428.0 | 52.1 | 567.9 | 393 |
| request-backpressure | 4096 | `zlink-java` | 104.767 | 3.939 | 6.355 | 8.518 | 56.4 | 4.51 | 870.6 | 32.6 | 981.9 | 506 |
| send-saturation | 1024 | `grpc-java` | 32.915 | 0.122 | 0.235 | 0.340 | 35.0 | 2.80 | 308.6 | 30.0 | 461.4 | 405 |
| send-saturation | 1024 | `zlink-c` | 527.256 | 12.843 | 3.640 | 3.903 | 13.9 | n/a | 11.2 | 5.2 | 22.2 | 2 |
| send-saturation | 1024 | `zlink-framework-java` | 106.351 | 0.226 | 0.245 | 4.451 | 27.8 | 2.23 | 333.6 | 35.8 | 653.2 | 542 |
| send-saturation | 1024 | `zlink-java` | 553.834 | 8.641 | 27.578 | 29.018 | 64.8 | 5.19 | 900.9 | 23.4 | 823.5 | 459 |
| send-saturation | 4096 | `grpc-java` | 31.933 | 0.126 | 0.226 | 0.348 | 34.7 | 2.78 | 424.4 | 28.6 | 446.1 | 414 |
| send-saturation | 4096 | `zlink-c` | 207.408 | 0.904 | 1.178 | 1.880 | 5.0 | n/a | 11.2 | 2.9 | 15.1 | 1 |
| send-saturation | 4096 | `zlink-framework-java` | 102.629 | 0.402 | 2.356 | 5.311 | 29.4 | 2.36 | 415.0 | 36.0 | 753.2 | 536 |
| send-saturation | 4096 | `zlink-java` | 203.852 | 3.344 | 19.713 | 23.271 | 50.4 | 4.03 | 951.7 | 23.6 | 808.3 | 504 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | saturated | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 1 | — | 0 | 1.0 | 349 | no | 1.49 | jvm_thread_cores | 0.123 | 1 | no | 41704 | 0 | — |
| request-serial | 1024 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 1024 | `zlink-framework-java` | 1 | — | 0 | 1.0 | 341 | no | 2.33 | jvm_thread_cores | 0.153 | 1 | no | 28208 | 0 | — |
| request-serial | 1024 | `zlink-java` | 1 | — | 0 | 1.0 | 357 | no | 0.71 | jvm_thread_cores | 0.239 | 1 | no | 68685 | 0 | — |
| request-serial | 4096 | `grpc-java` | 1 | — | 0 | 1.0 | 348 | no | 1.48 | jvm_thread_cores | 0.126 | 1 | no | 39791 | 0 | — |
| request-serial | 4096 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 4096 | `zlink-framework-java` | 1 | — | 0 | 1.0 | 350 | no | 2.66 | jvm_thread_cores | 0.153 | 1 | no | 22674 | 0 | — |
| request-serial | 4096 | `zlink-java` | 1 | — | 0 | 1.0 | 353 | no | 0.72 | jvm_thread_cores | 0.245 | 1 | no | 65756 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 15783 | — | 0 | 13899.5 | 21 | no | n/a | client_cores | n/a | — | not judged | n/a | 14031 | — |
| request-backpressure | 1024 | `zlink-framework-java` | 936 (low 818) | — | 0 | 140.9 | 399 | no | 4.91 | jvm_thread_cores | 1.062 | 8 | no | 188612 | 0 | — |
| request-backpressure | 1024 | `zlink-java` | 18861 (low 18261) | — | 0 | 13288.1 | 532 | no | 6.43 | jvm_thread_cores | 4.079 | 8 | no | 740775 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 922 (low 770) | — | 0 | 245.3 | 1 | no | n/a | client_cores | n/a | — | not judged | n/a | 205 | — |
| request-backpressure | 4096 | `zlink-framework-java` | 664 (low 546) | — | 0 | 114.4 | 393 | no | 4.72 | jvm_thread_cores | 1.078 | 8 | no | 178248 | 0 | — |
| request-backpressure | 4096 | `zlink-java` | 2057 (low 1762) | — | 0 | 412.7 | 506 | no | 4.51 | jvm_thread_cores | 1.955 | 8 | no | 524255 | 0 | — |
| send-saturation | 1024 | `grpc-java` | 8 | — | 0 | 4.0 | 405 | no | 2.80 | jvm_thread_cores | 0.470 | 8 | no | 164576 | 0 | server |
| send-saturation | 1024 | `zlink-c` | 8 | — | 0 | 6771.5 | 2 | no | n/a | client_cores | n/a | — | not judged | 2636278 | 0 | server |
| send-saturation | 1024 | `zlink-framework-java` | 8 | — | 0 | 24.0 | 542 | no | 2.23 | jvm_thread_cores | 1.130 | 8 | no | 531753 | 0 | server |
| send-saturation | 1024 | `zlink-java` | 11 (low 9) | — | 0 | 4785.6 | 459 | no | 5.19 | jvm_thread_cores | 3.786 | 8 | no | 2769172 | 0 | server |
| send-saturation | 4096 | `grpc-java` | 8 | — | 0 | 4.0 | 414 | no | 2.78 | jvm_thread_cores | 0.512 | 8 | no | 159666 | 0 | server |
| send-saturation | 4096 | `zlink-c` | 8 | — | 0 | 187.6 | 1 | no | n/a | client_cores | n/a | — | not judged | 1037039 | 0 | server |
| send-saturation | 4096 | `zlink-framework-java` | 8 | — | 0 | 41.3 | 536 | no | 2.36 | jvm_thread_cores | 1.176 | 8 | no | 513143 | 0 | server |
| send-saturation | 4096 | `zlink-java` | 11 (low 9) | — | 0 | 681.7 | 504 | no | 4.03 | jvm_thread_cores | 2.353 | 8 | no | 1019259 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 1 | 1 | http://127.0.0.1:5240/bench/start |
| request-serial | 1024 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 1024 | `zlink-framework-java` | 1 | 1 | http://127.0.0.1:5252/bench/start |
| request-serial | 1024 | `zlink-java` | 1 | 1 | http://127.0.0.1:5245/bench/start |
| request-serial | 4096 | `grpc-java` | 1 | 1 | http://127.0.0.1:5240/bench/start |
| request-serial | 4096 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 4096 | `zlink-framework-java` | 1 | 1 | http://127.0.0.1:5252/bench/start |
| request-serial | 4096 | `zlink-java` | 1 | 1 | http://127.0.0.1:5245/bench/start |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-framework-java` | 8 | unbounded | http://127.0.0.1:5252/bench/start |
| request-backpressure | 1024 | `zlink-java` | 8 | unbounded | http://127.0.0.1:5245/bench/start |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-framework-java` | 8 | unbounded | http://127.0.0.1:5252/bench/start |
| request-backpressure | 4096 | `zlink-java` | 8 | unbounded | http://127.0.0.1:5245/bench/start |
| send-saturation | 1024 | `grpc-java` | 8 | 1 | http://127.0.0.1:5240/bench/start |
| send-saturation | 1024 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 1024 | `zlink-framework-java` | 8 | 1 | http://127.0.0.1:5252/bench/start |
| send-saturation | 1024 | `zlink-java` | 8 | 1 | http://127.0.0.1:5245/bench/start |
| send-saturation | 4096 | `grpc-java` | 8 | 1 | http://127.0.0.1:5240/bench/start |
| send-saturation | 4096 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 4096 | `zlink-framework-java` | 8 | 1 | http://127.0.0.1:5252/bench/start |
| send-saturation | 4096 | `zlink-java` | 8 | 1 | http://127.0.0.1:5245/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 3 | 7.0% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 7.9% | pass |
| request-serial | 1024 | `zlink-framework-java` | 3 | 14.8% | **fail** |
| request-serial | 1024 | `zlink-java` | 3 | 1.2% | pass |
| request-serial | 4096 | `grpc-java` | 3 | 2.1% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 2.9% | pass |
| request-serial | 4096 | `zlink-framework-java` | 3 | 15.0% | **fail** |
| request-serial | 4096 | `zlink-java` | 3 | 3.5% | pass |
| request-backpressure | 1024 | `zlink-c` | 1 | n/a (1 run) | n/a |
| request-backpressure | 1024 | `zlink-framework-java` | 3 | 7.6% | pass |
| request-backpressure | 1024 | `zlink-java` | 3 | 1.4% | pass |
| request-backpressure | 4096 | `zlink-c` | 2 | 66.4% | n/a |
| request-backpressure | 4096 | `zlink-framework-java` | 3 | 13.0% | **fail** |
| request-backpressure | 4096 | `zlink-java` | 3 | 4.6% | pass |
| send-saturation | 1024 | `grpc-java` | 3 | 1.1% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 71.1% | **fail** |
| send-saturation | 1024 | `zlink-framework-java` | 3 | 1.0% | pass |
| send-saturation | 1024 | `zlink-java` | 3 | 1.3% | pass |
| send-saturation | 4096 | `grpc-java` | 3 | 1.4% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 64.0% | **fail** |
| send-saturation | 4096 | `zlink-framework-java` | 3 | 0.9% | pass |
| send-saturation | 4096 | `zlink-java` | 3 | 3.0% | pass |

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
| `zlink-java / zlink-c` | request-backpressure | 1024 | (1.140) | **unsupported** | — | denominator zlink-c-request-backpressure@1024 has 1 run(s); G5 needs 3; numerator zlink-java-request-backpressure@1024: warmup 2s, spec requires 30s; denominator zlink-c-request-backpressure@1024: warmup 2s, spec requires 30s (threshold 0.80) |
| `zlink-java / zlink-c` | request-backpressure | 4096 | (2.575) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 has 2 run(s); G5 needs 3; numerator zlink-java-request-backpressure@4096: warmup 2s, spec requires 30s; denominator zlink-c-request-backpressure@4096: warmup 2s, spec requires 30s (threshold 0.80) |
| `zlink-framework-java / zlink-java` | request-serial | 1024 | (0.411) | **unsupported** | — | numerator zlink-framework-java-request-serial@1024 fails G5 at 14.8% (limit 10%); numerator zlink-framework-java-request-serial@1024: warmup 2s, spec requires 30s; denominator zlink-java-request-serial@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-java / zlink-java` | request-serial | 4096 | (0.345) | **unsupported** | — | numerator zlink-framework-java-request-serial@4096 fails G5 at 15.0% (limit 10%); numerator zlink-framework-java-request-serial@4096: warmup 2s, spec requires 30s; denominator zlink-java-request-serial@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-java / zlink-java` | request-backpressure | 1024 | (0.260) | **unsupported** | — | numerator zlink-framework-java-request-backpressure@1024: warmup 2s, spec requires 30s; denominator zlink-java-request-backpressure@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-java / zlink-java` | request-backpressure | 4096 | (0.340) | **unsupported** | — | numerator zlink-framework-java-request-backpressure@4096 fails G5 at 13.0% (limit 10%); numerator zlink-framework-java-request-backpressure@4096: warmup 2s, spec requires 30s; denominator zlink-java-request-backpressure@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-java / zlink-java` | send-saturation | 1024 | (0.192) | **unsupported** | — | numerator zlink-framework-java-send-saturation@1024: warmup 2s, spec requires 30s; denominator zlink-java-send-saturation@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-java / zlink-java` | send-saturation | 4096 | (0.503) | **unsupported** | — | numerator zlink-framework-java-send-saturation@4096: warmup 2s, spec requires 30s; denominator zlink-java-send-saturation@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**java: incomplete** — 8 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 6 file(s)
- run2: read cell data from 6 file(s)
- run3: read cell data from 6 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)
