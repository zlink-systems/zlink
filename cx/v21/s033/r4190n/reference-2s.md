# #4190 참고표 — 2초 warmup, 정상 상태 아님 (판정은 30초 재측정 r4190w로 합니다)

- 원본 보고: r4190n/report.md (#4230 aggregator). 아래는 같은 raw를 G6 규칙(PR #4270) aggregator로 다시 집계한 판정입니다: r4190n/raw/aggregates-g6/{cpp,dotnet,java,node}.{md,json}
- G6 재집계에서 판정이 바뀐 것은 Node 6개 judgement뿐이고 C++·.NET·Java는 변화가 없습니다

## cpp
## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |        7.70 KOPS |    7.89 MB/s |     0.123 ms |     0.148 ms |     0.173 ms |       4.2% |    24.9 MB |       4.5% |    24.3 MB |
      | zlink-cpp               | 1024B    |       13.74 KOPS |   14.07 MB/s |     0.067 ms |     0.080 ms |     0.097 ms |       4.1% |    21.3 MB |       3.1% |    17.8 MB |
      | zlink-framework-cpp     | 1024B    |        8.82 KOPS |    9.03 MB/s |     0.107 ms |     0.134 ms |     0.163 ms |       4.2% |    30.3 MB |       4.0% |    26.9 MB |
      | grpc-cpp                | 4096B    |        7.35 KOPS |   30.10 MB/s |     0.129 ms |     0.155 ms |     0.181 ms |       4.2% |    25.1 MB |       4.5% |    24.7 MB |
      | zlink-cpp               | 4096B    |       12.87 KOPS |   52.73 MB/s |     0.072 ms |     0.086 ms |     0.107 ms |       4.0% |    21.5 MB |       3.1% |    17.9 MB |
      | zlink-framework-cpp     | 4096B    |        7.99 KOPS |   32.73 MB/s |     0.119 ms |     0.146 ms |     0.177 ms |       4.3% |    29.7 MB |       3.7% |    26.8 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      138.11 KOPS |  141.42 MB/s |   100.488 ms |   111.457 ms |   112.594 ms |      31.3% |    69.2 MB |       8.5% |    11.1 MB |
      | zlink-cpp               | 1024B    |      151.79 KOPS |  155.44 MB/s |    90.812 ms |    99.693 ms |   101.542 ms |      40.8% |    97.9 MB |      11.5% |    30.9 MB |
      | zlink-framework-cpp     | 1024B    |       47.54 KOPS |   48.68 MB/s |     5.385 ms |    15.876 ms |    17.623 ms |      29.0% |    38.6 MB |      20.2% |    36.4 MB |
      | zlink-c                 | 4096B    |      104.34 KOPS |  427.39 MB/s |     3.864 ms |     4.686 ms |     5.266 ms |      30.4% |    20.4 MB |      12.2% |    10.1 MB |
      | zlink-cpp               | 4096B    |      141.45 KOPS |  579.39 MB/s |     2.184 ms |     3.459 ms |     4.137 ms |      34.2% |    26.6 MB |      16.2% |    34.3 MB |
      | zlink-framework-cpp     | 4096B    |       45.81 KOPS |  187.62 MB/s |     3.224 ms |     8.498 ms |    10.887 ms |      29.8% |    37.6 MB |      20.0% |    34.7 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-cpp                | 1024B    |     24.39 KMSG/s |   24.98 MB/s |     0.165 ms |     0.243 ms |     0.283 ms |      12.1% |    25.9 MB |      13.7% |    29.6 MB |
      | zlink-cpp               | 1024B    |    592.11 KMSG/s |  606.32 MB/s |     1.225 ms |     1.473 ms |     2.417 ms |      30.5% |    23.6 MB |      10.6% |    55.3 MB |
      | zlink-framework-cpp     | 1024B    |    112.94 KMSG/s |  115.65 MB/s |     1.229 ms |     5.102 ms |     6.601 ms |      16.1% |    29.4 MB |      15.8% |    42.3 MB |
      | grpc-cpp                | 4096B    |     23.99 KMSG/s |   98.25 MB/s |     0.169 ms |     0.248 ms |     0.287 ms |      12.2% |    26.0 MB |      13.7% |    31.0 MB |
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
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,138108.000
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,141.423
RESULT,current,zlink-c-request-backpressure,local,1024,latency,100.488
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,111.457
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,112.594
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,31.331
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,69.221
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.485
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,11.086
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
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,104342.400
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,427.386
RESULT,current,zlink-c-request-backpressure,local,4096,latency,3.864
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,4.686
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,5.266
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,30.429
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,20.363
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,12.247
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,10.107
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
| request-serial | 1024 | `zlink-cpp` | 13.743 | 0.067 | 0.080 | 0.097 | 4.1 | 0.65 | 21.3 | 3.1 | 17.8 | 339 |
| request-serial | 1024 | `zlink-framework-cpp` | 8.822 | 0.107 | 0.134 | 0.163 | 4.2 | 0.68 | 30.3 | 4.0 | 26.9 | 336 |
| request-serial | 4096 | `grpc-cpp` | 7.348 | 0.129 | 0.155 | 0.181 | 4.2 | 0.67 | 25.1 | 4.5 | 24.7 | 332 |
| request-serial | 4096 | `zlink-cpp` | 12.873 | 0.072 | 0.086 | 0.107 | 4.0 | 0.64 | 21.5 | 3.1 | 17.9 | 344 |
| request-serial | 4096 | `zlink-framework-cpp` | 7.992 | 0.119 | 0.146 | 0.177 | 4.3 | 0.68 | 29.7 | 3.7 | 26.8 | 334 |
| request-backpressure | 1024 | `zlink-c` | 138.108 | 100.488 | 111.457 | 112.594 | 31.3 | n/a | 69.2 | 8.5 | 11.1 | 17 |
| request-backpressure | 1024 | `zlink-cpp` | 151.793 | 90.812 | 99.693 | 101.542 | 40.8 | 6.52 | 97.9 | 11.5 | 30.9 | 538 |
| request-backpressure | 1024 | `zlink-framework-cpp` | 47.540 | 5.385 | 15.876 | 17.623 | 29.0 | 4.63 | 38.6 | 20.2 | 36.4 | 397 |
| request-backpressure | 4096 | `zlink-c` | 104.342 | 3.864 | 4.686 | 5.266 | 30.4 | n/a | 20.4 | 12.2 | 10.1 | 0 |
| request-backpressure | 4096 | `zlink-cpp` | 141.454 | 2.184 | 3.459 | 4.137 | 34.2 | 5.48 | 26.6 | 16.2 | 34.3 | 529 |
| request-backpressure | 4096 | `zlink-framework-cpp` | 45.806 | 3.224 | 8.498 | 10.887 | 29.8 | 4.76 | 37.6 | 20.0 | 34.7 | 394 |
| send-saturation | 1024 | `grpc-cpp` | 24.393 | 0.165 | 0.243 | 0.283 | 12.1 | 1.94 | 25.9 | 13.7 | 29.6 | 358 |
| send-saturation | 1024 | `zlink-cpp` | 592.105 | 1.225 | 1.473 | 2.417 | 30.5 | 4.88 | 23.6 | 10.6 | 55.3 | 805 |
| send-saturation | 1024 | `zlink-framework-cpp` | 112.939 | 1.229 | 5.102 | 6.601 | 16.1 | 2.57 | 29.4 | 15.8 | 42.3 | 444 |
| send-saturation | 4096 | `grpc-cpp` | 23.987 | 0.169 | 0.248 | 0.287 | 12.2 | 1.95 | 26.0 | 13.7 | 31.0 | 351 |
| send-saturation | 4096 | `zlink-cpp` | 255.994 | 1.886 | 6.638 | 7.055 | 18.1 | 2.89 | 23.4 | 10.1 | 46.4 | 635 |
| send-saturation | 4096 | `zlink-framework-cpp` | 110.435 | 1.392 | 6.242 | 9.135 | 16.5 | 2.64 | 29.6 | 16.0 | 48.2 | 440 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | execution-model ceiling | G6 status | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | 0 | 0.9 | 330 | no | 0.67 | submit_thread_cores | 0.278 | 1 | 1 | no | 38521 | 0 | — |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | 0 | 0.9 | 339 | no | 0.65 | submit_thread_cores | 0.116 | 1 | 1 | no | 68717 | 0 | — |
| request-serial | 1024 | `zlink-framework-cpp` | 1 | 1 | 0 | 0.9 | 336 | no | 0.68 | submit_thread_cores | 0.136 | 1 | 1 | no | 44113 | 0 | — |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | 0 | 0.9 | 332 | no | 0.67 | submit_thread_cores | 0.281 | 1 | 1 | no | 36740 | 0 | — |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | 0 | 0.9 | 344 | no | 0.64 | submit_thread_cores | 0.128 | 1 | 1 | no | 64365 | 0 | — |
| request-serial | 4096 | `zlink-framework-cpp` | 1 | 1 | 0 | 0.9 | 334 | no | 0.68 | submit_thread_cores | 0.133 | 1 | 1 | no | 39959 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 16293 (low 15701) | — | 0 | 13878.2 | 17 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 1024 | `zlink-cpp` | 16767 (low 16532) | 0 | 0 | 13784.6 | 538 | no | 6.52 | submit_thread_cores | 4.597 | 8 | 8 | no | 773014 | 0 | — |
| request-backpressure | 1024 | `zlink-framework-cpp` | 779 (low 776) | 0 | 0 | 256.0 | 397 | no | 4.63 | submit_thread_cores | 1.477 | 8 | 8 | no | 237725 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 1189 (low 885) | — | 0 | 403.2 | 0 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 4096 | `zlink-cpp` | 1114 (low 954) | 0 | 0 | 308.9 | 529 | no | 5.48 | submit_thread_cores | 2.621 | 8 | 8 | no | 707691 | 0 | — |
| request-backpressure | 4096 | `zlink-framework-cpp` | 496 (low 479) | 0 | 0 | 147.7 | 394 | no | 4.76 | submit_thread_cores | 1.514 | 8 | 8 | no | 229080 | 0 | — |
| send-saturation | 1024 | `grpc-cpp` | 8 | 1 | 0 | 4.0 | 358 | no | 1.94 | submit_thread_cores | 0.790 | 8 | 8 | no | 121965 | 0 | server |
| send-saturation | 1024 | `zlink-cpp` | 8 | 1 | 0 | 725.6 | 805 | no | 4.88 | submit_thread_cores | 3.771 | 8 | 8 | no | 2960563 | 0 | server |
| send-saturation | 1024 | `zlink-framework-cpp` | 8 | 1 | 0 | 138.8 | 444 | no | 2.57 | submit_thread_cores | 1.562 | 8 | 8 | no | 564703 | 0 | server |
| send-saturation | 4096 | `grpc-cpp` | 8 | 1 | 0 | 4.0 | 351 | no | 1.95 | submit_thread_cores | 0.800 | 8 | 8 | no | 119936 | 0 | server |
| send-saturation | 4096 | `zlink-cpp` | 8 | 1 | 0 | 482.8 | 635 | no | 2.89 | submit_thread_cores | 1.752 | 8 | 8 | no | 1279988 | 0 | server |
| send-saturation | 4096 | `zlink-framework-cpp` | 8 | 1 | 0 | 153.7 | 440 | no | 2.64 | submit_thread_cores | 1.628 | 8 | 8 | no | 552183 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | http://127.0.0.1:5280/bench/start |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | http://127.0.0.1:5285/bench/start |
| request-serial | 1024 | `zlink-framework-cpp` | 1 | 1 | http://127.0.0.1:5292/bench/start |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | http://127.0.0.1:5280/bench/start |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | http://127.0.0.1:5285/bench/start |
| request-serial | 4096 | `zlink-framework-cpp` | 1 | 1 | http://127.0.0.1:5292/bench/start |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-cpp` | 8 | unbounded | http://127.0.0.1:5285/bench/start |
| request-backpressure | 1024 | `zlink-framework-cpp` | 8 | unbounded | http://127.0.0.1:5292/bench/start |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-cpp` | 8 | unbounded | http://127.0.0.1:5285/bench/start |
| request-backpressure | 4096 | `zlink-framework-cpp` | 8 | unbounded | http://127.0.0.1:5292/bench/start |
| send-saturation | 1024 | `grpc-cpp` | 8 | 1 | http://127.0.0.1:5280/bench/start |
| send-saturation | 1024 | `zlink-cpp` | 8 | 1 | http://127.0.0.1:5285/bench/start |
| send-saturation | 1024 | `zlink-framework-cpp` | 8 | 1 | http://127.0.0.1:5292/bench/start |
| send-saturation | 4096 | `grpc-cpp` | 8 | 1 | http://127.0.0.1:5280/bench/start |
| send-saturation | 4096 | `zlink-cpp` | 8 | 1 | http://127.0.0.1:5285/bench/start |
| send-saturation | 4096 | `zlink-framework-cpp` | 8 | 1 | http://127.0.0.1:5292/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 3 | 0.4% | pass |
| request-serial | 1024 | `zlink-cpp` | 3 | 2.4% | pass |
| request-serial | 1024 | `zlink-framework-cpp` | 3 | 8.2% | pass |
| request-serial | 4096 | `grpc-cpp` | 3 | 0.5% | pass |
| request-serial | 4096 | `zlink-cpp` | 3 | 2.4% | pass |
| request-serial | 4096 | `zlink-framework-cpp` | 3 | 7.6% | pass |
| request-backpressure | 1024 | `zlink-c` | 10 | 7.3% | pass |
| request-backpressure | 1024 | `zlink-cpp` | 3 | 1.3% | pass |
| request-backpressure | 1024 | `zlink-framework-cpp` | 3 | 1.8% | pass |
| request-backpressure | 4096 | `zlink-c` | 10 | 38.9% | **fail** |
| request-backpressure | 4096 | `zlink-cpp` | 3 | 5.5% | pass |
| request-backpressure | 4096 | `zlink-framework-cpp` | 3 | 1.5% | pass |
| send-saturation | 1024 | `grpc-cpp` | 3 | 0.6% | pass |
| send-saturation | 1024 | `zlink-cpp` | 3 | 0.4% | pass |
| send-saturation | 1024 | `zlink-framework-cpp` | 3 | 1.8% | pass |
| send-saturation | 4096 | `grpc-cpp` | 3 | 0.7% | pass |
| send-saturation | 4096 | `zlink-cpp` | 3 | 44.4% | **fail** |
| send-saturation | 4096 | `zlink-framework-cpp` | 3 | 1.2% | pass |

## Excluded cells (FB-008, excluded from tables and judgement)

None. No cell was contaminated or hit the drain bound.

## Incomplete server-driven cells (excluded from tables and judgement)

None. Every server-driven source has target stats.

## Judgement (spec 7.2)

| Formula | Pattern | Payload | Value | Status | Verdict | Reason |
|---|---:|---:|---:|---|---|---|
| `zlink-cpp / zlink-c` | request-backpressure | 1024 | 1.099 | published | pass | both rows pass G5 (threshold 0.80) |
| `zlink-cpp / zlink-c` | request-backpressure | 4096 | (1.356) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 fails G5 at 38.9% (limit 10%) (threshold 0.80) |
| `zlink-framework-cpp / zlink-cpp` | request-serial | 1024 | 0.642 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | request-serial | 4096 | 0.621 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | request-backpressure | 1024 | 0.313 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | request-backpressure | 4096 | 0.324 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | send-saturation | 1024 | 0.191 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-cpp / zlink-cpp` | send-saturation | 4096 | (0.431) | **unsupported** | — | denominator zlink-cpp-send-saturation@4096 fails G5 at 44.4% (limit 10%) (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**cpp: incomplete** — 2 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)

## dotnet
## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |        7.26 KOPS |    7.44 MB/s |     0.126 ms |     0.169 ms |     0.201 ms |      41.1% |   101.4 MB |      30.9% |   155.9 MB |
      | zlink-dotnet            | 1024B    |       10.19 KOPS |   10.43 MB/s |     0.090 ms |     0.112 ms |     0.134 ms |      28.6% |    97.7 MB |       5.5% |   157.2 MB |
      | zlink-framework-dotnet  | 1024B    |        6.15 KOPS |    6.30 MB/s |     0.153 ms |     0.193 ms |     0.234 ms |      23.5% |   127.2 MB |       7.2% |   192.0 MB |
      | grpc-dotnet             | 4096B    |        6.34 KOPS |   25.97 MB/s |     0.143 ms |     0.180 ms |     0.213 ms |      39.1% |   105.5 MB |      29.0% |   161.0 MB |
      | zlink-dotnet            | 4096B    |        9.47 KOPS |   38.77 MB/s |     0.094 ms |     0.117 ms |     0.144 ms |      29.2% |    96.8 MB |       5.2% |   166.0 MB |
      | zlink-framework-dotnet  | 4096B    |        5.94 KOPS |   24.34 MB/s |     0.156 ms |     0.196 ms |     0.242 ms |      24.0% |   128.5 MB |       6.9% |   191.7 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      138.11 KOPS |  141.42 MB/s |   100.488 ms |   111.457 ms |   112.594 ms |      31.3% |    69.2 MB |       8.5% |    11.1 MB |
      | zlink-dotnet            | 1024B    |      105.16 KOPS |  107.69 MB/s |    44.880 ms |    67.046 ms |    75.721 ms |      66.9% |   169.7 MB |      14.9% |   166.3 MB |
      | zlink-framework-dotnet  | 1024B    |       47.12 KOPS |   48.25 MB/s |     5.997 ms |    18.697 ms |    25.653 ms |      75.7% |   157.2 MB |      27.3% |   204.9 MB |
      | zlink-c                 | 4096B    |      104.34 KOPS |  427.39 MB/s |     3.864 ms |     4.686 ms |     5.266 ms |      30.4% |    20.4 MB |      12.2% |    10.1 MB |
      | zlink-dotnet            | 4096B    |       98.00 KOPS |  401.39 MB/s |     3.597 ms |     5.681 ms |     6.890 ms |      68.8% |   130.1 MB |      23.8% |   171.1 MB |
      | zlink-framework-dotnet  | 4096B    |       38.29 KOPS |  156.82 MB/s |     3.413 ms |     8.598 ms |    10.577 ms |      73.7% |   153.5 MB |      26.0% |   204.4 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |     23.23 KMSG/s |   23.79 MB/s |     0.171 ms |     0.264 ms |     0.382 ms |      93.4% |   106.7 MB |      69.9% |   168.8 MB |
      | zlink-dotnet            | 1024B    |    436.67 KMSG/s |  447.15 MB/s |     1.647 ms |     3.696 ms |     9.340 ms |      75.9% |   127.4 MB |      21.4% |   203.2 MB |
      | zlink-framework-dotnet  | 1024B    |    194.20 KMSG/s |  198.86 MB/s |     4.108 ms |     9.166 ms |    13.585 ms |      46.2% |   137.1 MB |      33.0% |   225.9 MB |
      | grpc-dotnet             | 4096B    |     21.60 KMSG/s |   88.46 MB/s |     0.189 ms |     0.289 ms |     0.429 ms |      91.8% |   111.5 MB |      71.5% |   176.6 MB |
      | zlink-dotnet            | 4096B    |    181.43 KMSG/s |  743.16 MB/s |     1.079 ms |     1.672 ms |     4.251 ms |      62.8% |   119.8 MB |      21.9% |   196.8 MB |
      | zlink-framework-dotnet  | 4096B    |    136.05 KMSG/s |  557.26 MB/s |     3.285 ms |     7.839 ms |    15.369 ms |      54.9% |   138.5 MB |      35.0% |   229.5 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-dotnet-request-serial,local,1024,throughput,7261.200
RESULT,current,grpc-dotnet-request-serial,local,1024,bandwidth,7.435
RESULT,current,grpc-dotnet-request-serial,local,1024,latency,0.126
RESULT,current,grpc-dotnet-request-serial,local,1024,latency_p95,0.169
RESULT,current,grpc-dotnet-request-serial,local,1024,latency_p99,0.201
RESULT,current,grpc-dotnet-request-serial,local,1024,client_cpu_percent,41.050
RESULT,current,grpc-dotnet-request-serial,local,1024,client_memory_mb,101.367
RESULT,current,grpc-dotnet-request-serial,local,1024,server_cpu_percent,30.950
RESULT,current,grpc-dotnet-request-serial,local,1024,server_memory_mb,155.852
RESULT,current,zlink-dotnet-request-serial,local,1024,throughput,10185.800
RESULT,current,zlink-dotnet-request-serial,local,1024,bandwidth,10.430
RESULT,current,zlink-dotnet-request-serial,local,1024,latency,0.090
RESULT,current,zlink-dotnet-request-serial,local,1024,latency_p95,0.112
RESULT,current,zlink-dotnet-request-serial,local,1024,latency_p99,0.134
RESULT,current,zlink-dotnet-request-serial,local,1024,client_cpu_percent,28.575
RESULT,current,zlink-dotnet-request-serial,local,1024,client_memory_mb,97.676
RESULT,current,zlink-dotnet-request-serial,local,1024,server_cpu_percent,5.475
RESULT,current,zlink-dotnet-request-serial,local,1024,server_memory_mb,157.230
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,throughput,6152.800
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,bandwidth,6.300
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency,0.153
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency_p95,0.193
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency_p99,0.234
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,client_cpu_percent,23.450
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,client_memory_mb,127.234
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,server_cpu_percent,7.200
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,server_memory_mb,191.965
RESULT,current,grpc-dotnet-request-serial,local,4096,throughput,6339.800
RESULT,current,grpc-dotnet-request-serial,local,4096,bandwidth,25.968
RESULT,current,grpc-dotnet-request-serial,local,4096,latency,0.143
RESULT,current,grpc-dotnet-request-serial,local,4096,latency_p95,0.180
RESULT,current,grpc-dotnet-request-serial,local,4096,latency_p99,0.213
RESULT,current,grpc-dotnet-request-serial,local,4096,client_cpu_percent,39.125
RESULT,current,grpc-dotnet-request-serial,local,4096,client_memory_mb,105.469
RESULT,current,grpc-dotnet-request-serial,local,4096,server_cpu_percent,29.000
RESULT,current,grpc-dotnet-request-serial,local,4096,server_memory_mb,160.957
RESULT,current,zlink-dotnet-request-serial,local,4096,throughput,9465.400
RESULT,current,zlink-dotnet-request-serial,local,4096,bandwidth,38.770
RESULT,current,zlink-dotnet-request-serial,local,4096,latency,0.094
RESULT,current,zlink-dotnet-request-serial,local,4096,latency_p95,0.117
RESULT,current,zlink-dotnet-request-serial,local,4096,latency_p99,0.144
RESULT,current,zlink-dotnet-request-serial,local,4096,client_cpu_percent,29.200
RESULT,current,zlink-dotnet-request-serial,local,4096,client_memory_mb,96.844
RESULT,current,zlink-dotnet-request-serial,local,4096,server_cpu_percent,5.150
RESULT,current,zlink-dotnet-request-serial,local,4096,server_memory_mb,165.969
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,throughput,5942.800
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,bandwidth,24.342
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency,0.156
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency_p95,0.196
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency_p99,0.242
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,client_cpu_percent,23.975
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,client_memory_mb,128.488
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,server_cpu_percent,6.875
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,server_memory_mb,191.711
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,138108.000
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,141.423
RESULT,current,zlink-c-request-backpressure,local,1024,latency,100.488
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,111.457
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,112.594
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,31.331
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,69.221
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.485
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,11.086
RESULT,current,zlink-dotnet-request-backpressure,local,1024,throughput,105162.400
RESULT,current,zlink-dotnet-request-backpressure,local,1024,bandwidth,107.686
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency,44.880
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency_p95,67.046
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency_p99,75.721
RESULT,current,zlink-dotnet-request-backpressure,local,1024,client_cpu_percent,66.900
RESULT,current,zlink-dotnet-request-backpressure,local,1024,client_memory_mb,169.664
RESULT,current,zlink-dotnet-request-backpressure,local,1024,server_cpu_percent,14.925
RESULT,current,zlink-dotnet-request-backpressure,local,1024,server_memory_mb,166.281
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,throughput,47115.800
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,bandwidth,48.247
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency,5.997
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency_p95,18.697
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency_p99,25.653
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,client_cpu_percent,75.675
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,client_memory_mb,157.152
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,server_cpu_percent,27.250
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,server_memory_mb,204.852
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,104342.400
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,427.386
RESULT,current,zlink-c-request-backpressure,local,4096,latency,3.864
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,4.686
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,5.266
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,30.429
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,20.363
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,12.247
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,10.107
RESULT,current,zlink-dotnet-request-backpressure,local,4096,throughput,97996.800
RESULT,current,zlink-dotnet-request-backpressure,local,4096,bandwidth,401.395
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency,3.597
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency_p95,5.681
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency_p99,6.890
RESULT,current,zlink-dotnet-request-backpressure,local,4096,client_cpu_percent,68.775
RESULT,current,zlink-dotnet-request-backpressure,local,4096,client_memory_mb,130.137
RESULT,current,zlink-dotnet-request-backpressure,local,4096,server_cpu_percent,23.825
RESULT,current,zlink-dotnet-request-backpressure,local,4096,server_memory_mb,171.129
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,throughput,38287.000
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,bandwidth,156.824
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency,3.413
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency_p95,8.598
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency_p99,10.577
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,client_cpu_percent,73.700
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,client_memory_mb,153.473
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,server_cpu_percent,26.025
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,server_memory_mb,204.445
RESULT,current,grpc-dotnet-send-saturation,local,1024,throughput,23231.200
RESULT,current,grpc-dotnet-send-saturation,local,1024,bandwidth,23.789
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency,0.171
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency_p95,0.264
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency_p99,0.382
RESULT,current,grpc-dotnet-send-saturation,local,1024,client_cpu_percent,93.375
RESULT,current,grpc-dotnet-send-saturation,local,1024,client_memory_mb,106.730
RESULT,current,grpc-dotnet-send-saturation,local,1024,server_cpu_percent,69.925
RESULT,current,grpc-dotnet-send-saturation,local,1024,server_memory_mb,168.805
RESULT,current,zlink-dotnet-send-saturation,local,1024,throughput,436668.000
RESULT,current,zlink-dotnet-send-saturation,local,1024,bandwidth,447.148
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency,1.647
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency_p95,3.696
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency_p99,9.340
RESULT,current,zlink-dotnet-send-saturation,local,1024,client_cpu_percent,75.875
RESULT,current,zlink-dotnet-send-saturation,local,1024,client_memory_mb,127.387
RESULT,current,zlink-dotnet-send-saturation,local,1024,server_cpu_percent,21.450
RESULT,current,zlink-dotnet-send-saturation,local,1024,server_memory_mb,203.246
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,throughput,194201.600
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,bandwidth,198.862
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency,4.108
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency_p95,9.166
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency_p99,13.585
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,client_cpu_percent,46.175
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,client_memory_mb,137.090
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,server_cpu_percent,32.950
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,server_memory_mb,225.934
RESULT,current,grpc-dotnet-send-saturation,local,4096,throughput,21596.600
RESULT,current,grpc-dotnet-send-saturation,local,4096,bandwidth,88.460
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency,0.189
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency_p95,0.289
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency_p99,0.429
RESULT,current,grpc-dotnet-send-saturation,local,4096,client_cpu_percent,91.800
RESULT,current,grpc-dotnet-send-saturation,local,4096,client_memory_mb,111.469
RESULT,current,grpc-dotnet-send-saturation,local,4096,server_cpu_percent,71.550
RESULT,current,grpc-dotnet-send-saturation,local,4096,server_memory_mb,176.641
RESULT,current,zlink-dotnet-send-saturation,local,4096,throughput,181434.800
RESULT,current,zlink-dotnet-send-saturation,local,4096,bandwidth,743.157
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency,1.079
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency_p95,1.672
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency_p99,4.251
RESULT,current,zlink-dotnet-send-saturation,local,4096,client_cpu_percent,62.800
RESULT,current,zlink-dotnet-send-saturation,local,4096,client_memory_mb,119.844
RESULT,current,zlink-dotnet-send-saturation,local,4096,server_cpu_percent,21.900
RESULT,current,zlink-dotnet-send-saturation,local,4096,server_memory_mb,196.797
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,throughput,136050.600
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,bandwidth,557.263
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency,3.285
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency_p95,7.839
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency_p99,15.369
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,client_cpu_percent,54.850
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,client_memory_mb,138.543
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,server_cpu_percent,35.050
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,server_memory_mb,229.543


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 7.261 | 0.126 | 0.169 | 0.201 | 41.1 | 3.28 | 101.4 | 30.9 | 155.9 | 330 |
| request-serial | 1024 | `zlink-dotnet` | 10.186 | 0.090 | 0.112 | 0.134 | 28.6 | 2.29 | 97.7 | 5.5 | 157.2 | 330 |
| request-serial | 1024 | `zlink-framework-dotnet` | 6.153 | 0.153 | 0.193 | 0.234 | 23.5 | 1.88 | 127.2 | 7.2 | 192.0 | 339 |
| request-serial | 4096 | `grpc-dotnet` | 6.340 | 0.143 | 0.180 | 0.213 | 39.1 | 3.13 | 105.5 | 29.0 | 161.0 | 328 |
| request-serial | 4096 | `zlink-dotnet` | 9.465 | 0.094 | 0.117 | 0.144 | 29.2 | 2.34 | 96.8 | 5.2 | 166.0 | 338 |
| request-serial | 4096 | `zlink-framework-dotnet` | 5.943 | 0.156 | 0.196 | 0.242 | 24.0 | 1.92 | 128.5 | 6.9 | 191.7 | 331 |
| request-backpressure | 1024 | `zlink-c` | 138.108 | 100.488 | 111.457 | 112.594 | 31.3 | n/a | 69.2 | 8.5 | 11.1 | 17 |
| request-backpressure | 1024 | `zlink-dotnet` | 105.162 | 44.880 | 67.046 | 75.721 | 66.9 | 5.35 | 169.7 | 14.9 | 166.3 | 330 |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 47.116 | 5.997 | 18.697 | 25.653 | 75.7 | 6.05 | 157.2 | 27.3 | 204.9 | 330 |
| request-backpressure | 4096 | `zlink-c` | 104.342 | 3.864 | 4.686 | 5.266 | 30.4 | n/a | 20.4 | 12.2 | 10.1 | 0 |
| request-backpressure | 4096 | `zlink-dotnet` | 97.997 | 3.597 | 5.681 | 6.890 | 68.8 | 5.50 | 130.1 | 23.8 | 171.1 | 330 |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 38.287 | 3.413 | 8.598 | 10.577 | 73.7 | 5.90 | 153.5 | 26.0 | 204.4 | 329 |
| send-saturation | 1024 | `grpc-dotnet` | 23.231 | 0.171 | 0.264 | 0.382 | 93.4 | 7.47 | 106.7 | 69.9 | 168.8 | 352 |
| send-saturation | 1024 | `zlink-dotnet` | 436.668 | 1.647 | 3.696 | 9.340 | 75.9 | 6.07 | 127.4 | 21.4 | 203.2 | 637 |
| send-saturation | 1024 | `zlink-framework-dotnet` | 194.202 | 4.108 | 9.166 | 13.585 | 46.2 | 3.69 | 137.1 | 33.0 | 225.9 | 455 |
| send-saturation | 4096 | `grpc-dotnet` | 21.597 | 0.189 | 0.289 | 0.429 | 91.8 | 7.34 | 111.5 | 71.5 | 176.6 | 379 |
| send-saturation | 4096 | `zlink-dotnet` | 181.435 | 1.079 | 1.672 | 4.251 | 62.8 | 5.02 | 119.8 | 21.9 | 196.8 | 467 |
| send-saturation | 4096 | `zlink-framework-dotnet` | 136.051 | 3.285 | 7.839 | 15.369 | 54.9 | 4.39 | 138.5 | 35.0 | 229.5 | 427 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | execution-model ceiling | G6 status | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 1 | — | 0 | 0.9 | 330 | no | 3.28 | client_cores | 3.284 | 8 | 1 | no | 36307 | 0 | — |
| request-serial | 1024 | `zlink-dotnet` | 1 | — | 0 | 0.9 | 330 | no | 2.29 | client_cores | 2.286 | 8 | 1 | no | 50930 | 0 | — |
| request-serial | 1024 | `zlink-framework-dotnet` | 1 | — | 0 | 0.9 | 339 | no | 1.88 | client_cores | 1.876 | 8 | 1 | no | 30765 | 0 | — |
| request-serial | 4096 | `grpc-dotnet` | 1 | — | 0 | 0.9 | 328 | no | 3.13 | client_cores | 3.130 | 8 | 1 | no | 31700 | 0 | — |
| request-serial | 4096 | `zlink-dotnet` | 1 | — | 0 | 0.9 | 338 | no | 2.34 | client_cores | 2.336 | 8 | 1 | no | 47328 | 0 | — |
| request-serial | 4096 | `zlink-framework-dotnet` | 1 | — | 0 | 0.9 | 331 | no | 1.92 | client_cores | 1.918 | 8 | 1 | no | 29715 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 16293 (low 15701) | — | 0 | 13878.2 | 17 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 1024 | `zlink-dotnet` | 7930 (low 7220) | — | 0 | 4719.7 | 330 | no | 5.35 | client_cores | 5.352 | 8 | 8 | no | 531083 | 0 | — |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 2416 (low 1948) | — | 0 | 282.5 | 330 | no | 6.05 | client_cores | 6.054 | 8 | 8 | no | 236025 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 1189 (low 885) | — | 0 | 403.2 | 0 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 4096 | `zlink-dotnet` | 1578 (low 1326) | — | 0 | 352.5 | 330 | no | 5.50 | client_cores | 5.502 | 8 | 8 | no | 490388 | 0 | — |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 794 (low 710) | — | 0 | 130.7 | 329 | no | 5.90 | client_cores | 5.896 | 8 | 8 | no | 191703 | 0 | — |
| send-saturation | 1024 | `grpc-dotnet` | 8 | — | 0 | 4.0 | 352 | no | 7.47 | client_cores | 7.470 | 8 | 8 | no | 116156 | 0 | server |
| send-saturation | 1024 | `zlink-dotnet` | 8 | — | 0 | 719.1 | 637 | no | 6.07 | client_cores | 6.070 | 8 | 8 | no | 2183340 | 0 | server |
| send-saturation | 1024 | `zlink-framework-dotnet` | 8 | — | 0 | 797.7 | 455 | no | 3.69 | client_cores | 3.694 | 8 | 8 | no | 971008 | 0 | server |
| send-saturation | 4096 | `grpc-dotnet` | 8 | — | 0 | 4.1 | 379 | no | 7.34 | client_cores | 7.344 | 8 | 8 | no | 107983 | 0 | server |
| send-saturation | 4096 | `zlink-dotnet` | 8 | — | 0 | 195.7 | 467 | no | 5.02 | client_cores | 5.024 | 8 | 8 | no | 907174 | 0 | server |
| send-saturation | 4096 | `zlink-framework-dotnet` | 8 | — | 0 | 446.9 | 427 | no | 4.39 | client_cores | 4.388 | 8 | 8 | no | 680253 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 1 | 1 | http://127.0.0.1:5200 |
| request-serial | 1024 | `zlink-dotnet` | 1 | 1 | http://127.0.0.1:5205 |
| request-serial | 1024 | `zlink-framework-dotnet` | 1 | 1 | http://127.0.0.1:5212 |
| request-serial | 4096 | `grpc-dotnet` | 1 | 1 | http://127.0.0.1:5200 |
| request-serial | 4096 | `zlink-dotnet` | 1 | 1 | http://127.0.0.1:5205 |
| request-serial | 4096 | `zlink-framework-dotnet` | 1 | 1 | http://127.0.0.1:5212 |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-dotnet` | 8 | unbounded | http://127.0.0.1:5205 |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 8 | unbounded | http://127.0.0.1:5212 |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-dotnet` | 8 | unbounded | http://127.0.0.1:5205 |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 8 | unbounded | http://127.0.0.1:5212 |
| send-saturation | 1024 | `grpc-dotnet` | 8 | 1 | http://127.0.0.1:5200 |
| send-saturation | 1024 | `zlink-dotnet` | 8 | 1 | http://127.0.0.1:5205 |
| send-saturation | 1024 | `zlink-framework-dotnet` | 8 | 1 | http://127.0.0.1:5212 |
| send-saturation | 4096 | `grpc-dotnet` | 8 | 1 | http://127.0.0.1:5200 |
| send-saturation | 4096 | `zlink-dotnet` | 8 | 1 | http://127.0.0.1:5205 |
| send-saturation | 4096 | `zlink-framework-dotnet` | 8 | 1 | http://127.0.0.1:5212 |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 3 | 0.4% | pass |
| request-serial | 1024 | `zlink-dotnet` | 3 | 7.2% | pass |
| request-serial | 1024 | `zlink-framework-dotnet` | 3 | 1.6% | pass |
| request-serial | 4096 | `grpc-dotnet` | 3 | 3.2% | pass |
| request-serial | 4096 | `zlink-dotnet` | 3 | 6.9% | pass |
| request-serial | 4096 | `zlink-framework-dotnet` | 3 | 1.7% | pass |
| request-backpressure | 1024 | `zlink-c` | 10 | 7.3% | pass |
| request-backpressure | 1024 | `zlink-dotnet` | 3 | 9.1% | pass |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 3 | 5.9% | pass |
| request-backpressure | 4096 | `zlink-c` | 10 | 38.9% | **fail** |
| request-backpressure | 4096 | `zlink-dotnet` | 3 | 2.5% | pass |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 3 | 2.3% | pass |
| send-saturation | 1024 | `grpc-dotnet` | 3 | 1.4% | pass |
| send-saturation | 1024 | `zlink-dotnet` | 3 | 1.2% | pass |
| send-saturation | 1024 | `zlink-framework-dotnet` | 3 | 2.2% | pass |
| send-saturation | 4096 | `grpc-dotnet` | 3 | 1.2% | pass |
| send-saturation | 4096 | `zlink-dotnet` | 3 | 0.9% | pass |
| send-saturation | 4096 | `zlink-framework-dotnet` | 3 | 3.0% | pass |

## Excluded cells (FB-008, excluded from tables and judgement)

None. No cell was contaminated or hit the drain bound.

## Incomplete server-driven cells (excluded from tables and judgement)

None. Every server-driven source has target stats.

## Judgement (spec 7.2)

| Formula | Pattern | Payload | Value | Status | Verdict | Reason |
|---|---:|---:|---:|---|---|---|
| `zlink-dotnet / zlink-c` | request-backpressure | 1024 | 0.761 | published | **fail** | both rows pass G5 (threshold 0.80) |
| `zlink-dotnet / zlink-c` | request-backpressure | 4096 | (0.939) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 fails G5 at 38.9% (limit 10%) (threshold 0.80) |
| `zlink-framework-dotnet / zlink-dotnet` | request-serial | 1024 | 0.604 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-serial | 4096 | 0.628 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-backpressure | 1024 | 0.448 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-backpressure | 4096 | 0.391 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | send-saturation | 1024 | 0.445 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | send-saturation | 4096 | 0.750 | published | pass | both rows pass G5 (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**dotnet: incomplete** — 1 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)

## java
## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-java               | 1024B    |        8.34 KOPS |    8.54 MB/s |     0.119 ms |     0.233 ms |     0.306 ms |      18.6% |   256.3 MB |      17.3% |   373.1 MB |
      | zlink-framework-java    | 1024B    |        5.64 KOPS |    5.78 MB/s |     0.176 ms |     0.322 ms |     0.408 ms |      29.1% |   312.6 MB |      24.9% |   315.1 MB |
      | zlink-java              | 1024B    |       13.74 KOPS |   14.07 MB/s |     0.073 ms |     0.093 ms |     0.113 ms |       8.9% |   449.2 MB |       7.2% |   341.8 MB |
      | grpc-java               | 4096B    |        7.96 KOPS |   32.60 MB/s |     0.125 ms |     0.252 ms |     0.337 ms |      18.4% |   242.4 MB |      17.0% |   446.0 MB |
      | zlink-framework-java    | 4096B    |        4.53 KOPS |   18.57 MB/s |     0.220 ms |     0.350 ms |     0.424 ms |      33.2% |   311.4 MB |      25.2% |   315.1 MB |
      | zlink-java              | 4096B    |       13.15 KOPS |   53.87 MB/s |     0.076 ms |     0.094 ms |     0.116 ms |       9.0% |   434.8 MB |       7.2% |   410.2 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      138.11 KOPS |  141.42 MB/s |   100.488 ms |   111.457 ms |   112.594 ms |      31.3% |    69.2 MB |       8.5% |    11.1 MB |
      | zlink-framework-java    | 1024B    |       37.72 KOPS |   38.62 MB/s |     3.737 ms |    21.743 ms |    30.582 ms |      61.4% |   400.9 MB |      43.9% |   512.7 MB |
      | zlink-java              | 1024B    |      145.33 KOPS |  148.82 MB/s |    91.433 ms |   114.958 ms |   132.139 ms |      80.4% |   841.2 MB |      28.6% |   667.7 MB |
      | zlink-c                 | 4096B    |      104.34 KOPS |  427.39 MB/s |     3.864 ms |     4.686 ms |     5.266 ms |      30.4% |    20.4 MB |      12.2% |    10.1 MB |
      | zlink-framework-java    | 4096B    |       35.65 KOPS |  146.00 MB/s |     3.208 ms |    13.681 ms |    22.992 ms |      58.9% |   428.0 MB |      52.1% |   567.9 MB |
      | zlink-java              | 4096B    |      104.77 KOPS |  429.13 MB/s |     3.939 ms |     6.355 ms |     8.518 ms |      56.4% |   870.6 MB |      32.6% |   981.9 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-java               | 1024B    |     32.92 KMSG/s |   33.71 MB/s |     0.122 ms |     0.235 ms |     0.340 ms |      35.0% |   308.6 MB |      30.0% |   461.4 MB |
      | zlink-framework-java    | 1024B    |    106.35 KMSG/s |  108.90 MB/s |     0.226 ms |     0.245 ms |     4.451 ms |      27.8% |   333.6 MB |      35.8% |   653.2 MB |
      | zlink-java              | 1024B    |    553.83 KMSG/s |  567.13 MB/s |     8.641 ms |    27.578 ms |    29.018 ms |      64.8% |   900.9 MB |      23.4% |   823.5 MB |
      | grpc-java               | 4096B    |     31.93 KMSG/s |  130.80 MB/s |     0.126 ms |     0.226 ms |     0.348 ms |      34.7% |   424.4 MB |      28.6% |   446.1 MB |
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
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,138108.000
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,141.423
RESULT,current,zlink-c-request-backpressure,local,1024,latency,100.488
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,111.457
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,112.594
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,31.331
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,69.221
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.485
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,11.086
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
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,104342.400
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,427.386
RESULT,current,zlink-c-request-backpressure,local,4096,latency,3.864
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,4.686
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,5.266
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,30.429
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,20.363
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,12.247
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,10.107
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
| request-serial | 1024 | `zlink-framework-java` | 5.641 | 0.176 | 0.322 | 0.408 | 29.1 | 2.33 | 312.6 | 24.9 | 315.1 | 341 |
| request-serial | 1024 | `zlink-java` | 13.737 | 0.073 | 0.093 | 0.113 | 8.9 | 0.71 | 449.2 | 7.2 | 341.8 | 357 |
| request-serial | 4096 | `grpc-java` | 7.958 | 0.125 | 0.252 | 0.337 | 18.4 | 1.48 | 242.4 | 17.0 | 446.0 | 348 |
| request-serial | 4096 | `zlink-framework-java` | 4.535 | 0.220 | 0.350 | 0.424 | 33.2 | 2.66 | 311.4 | 25.2 | 315.1 | 350 |
| request-serial | 4096 | `zlink-java` | 13.151 | 0.076 | 0.094 | 0.116 | 9.0 | 0.72 | 434.8 | 7.2 | 410.2 | 353 |
| request-backpressure | 1024 | `zlink-c` | 138.108 | 100.488 | 111.457 | 112.594 | 31.3 | n/a | 69.2 | 8.5 | 11.1 | 17 |
| request-backpressure | 1024 | `zlink-framework-java` | 37.719 | 3.737 | 21.743 | 30.582 | 61.4 | 4.91 | 400.9 | 43.9 | 512.7 | 399 |
| request-backpressure | 1024 | `zlink-java` | 145.331 | 91.433 | 114.958 | 132.139 | 80.4 | 6.43 | 841.2 | 28.6 | 667.7 | 532 |
| request-backpressure | 4096 | `zlink-c` | 104.342 | 3.864 | 4.686 | 5.266 | 30.4 | n/a | 20.4 | 12.2 | 10.1 | 0 |
| request-backpressure | 4096 | `zlink-framework-java` | 35.645 | 3.208 | 13.681 | 22.992 | 58.9 | 4.72 | 428.0 | 52.1 | 567.9 | 393 |
| request-backpressure | 4096 | `zlink-java` | 104.767 | 3.939 | 6.355 | 8.518 | 56.4 | 4.51 | 870.6 | 32.6 | 981.9 | 506 |
| send-saturation | 1024 | `grpc-java` | 32.915 | 0.122 | 0.235 | 0.340 | 35.0 | 2.80 | 308.6 | 30.0 | 461.4 | 405 |
| send-saturation | 1024 | `zlink-framework-java` | 106.351 | 0.226 | 0.245 | 4.451 | 27.8 | 2.23 | 333.6 | 35.8 | 653.2 | 542 |
| send-saturation | 1024 | `zlink-java` | 553.834 | 8.641 | 27.578 | 29.018 | 64.8 | 5.19 | 900.9 | 23.4 | 823.5 | 459 |
| send-saturation | 4096 | `grpc-java` | 31.933 | 0.126 | 0.226 | 0.348 | 34.7 | 2.78 | 424.4 | 28.6 | 446.1 | 414 |
| send-saturation | 4096 | `zlink-framework-java` | 102.629 | 0.402 | 2.356 | 5.311 | 29.4 | 2.36 | 415.0 | 36.0 | 753.2 | 536 |
| send-saturation | 4096 | `zlink-java` | 203.852 | 3.344 | 19.713 | 23.271 | 50.4 | 4.03 | 951.7 | 23.6 | 808.3 | 504 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | execution-model ceiling | G6 status | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 1 | — | 0 | 1.0 | 349 | no | 1.49 | jvm_thread_cores | 0.123 | 1 | 1 | no | 41704 | 0 | — |
| request-serial | 1024 | `zlink-framework-java` | 1 | — | 0 | 1.0 | 341 | no | 2.33 | jvm_thread_cores | 0.153 | 1 | 1 | no | 28208 | 0 | — |
| request-serial | 1024 | `zlink-java` | 1 | — | 0 | 1.0 | 357 | no | 0.71 | jvm_thread_cores | 0.239 | 1 | 1 | no | 68685 | 0 | — |
| request-serial | 4096 | `grpc-java` | 1 | — | 0 | 1.0 | 348 | no | 1.48 | jvm_thread_cores | 0.126 | 1 | 1 | no | 39791 | 0 | — |
| request-serial | 4096 | `zlink-framework-java` | 1 | — | 0 | 1.0 | 350 | no | 2.66 | jvm_thread_cores | 0.153 | 1 | 1 | no | 22674 | 0 | — |
| request-serial | 4096 | `zlink-java` | 1 | — | 0 | 1.0 | 353 | no | 0.72 | jvm_thread_cores | 0.245 | 1 | 1 | no | 65756 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 16293 (low 15701) | — | 0 | 13878.2 | 17 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 1024 | `zlink-framework-java` | 936 (low 818) | — | 0 | 140.9 | 399 | no | 4.91 | jvm_thread_cores | 1.062 | 8 | 8 | no | 188612 | 0 | — |
| request-backpressure | 1024 | `zlink-java` | 18861 (low 18261) | — | 0 | 13288.1 | 532 | no | 6.43 | jvm_thread_cores | 4.079 | 8 | 8 | no | 740775 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 1189 (low 885) | — | 0 | 403.2 | 0 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 4096 | `zlink-framework-java` | 664 (low 546) | — | 0 | 114.4 | 393 | no | 4.72 | jvm_thread_cores | 1.078 | 8 | 8 | no | 178248 | 0 | — |
| request-backpressure | 4096 | `zlink-java` | 2057 (low 1762) | — | 0 | 412.7 | 506 | no | 4.51 | jvm_thread_cores | 1.955 | 8 | 8 | no | 524255 | 0 | — |
| send-saturation | 1024 | `grpc-java` | 8 | — | 0 | 4.0 | 405 | no | 2.80 | jvm_thread_cores | 0.470 | 8 | 8 | no | 164576 | 0 | server |
| send-saturation | 1024 | `zlink-framework-java` | 8 | — | 0 | 24.0 | 542 | no | 2.23 | jvm_thread_cores | 1.130 | 8 | 8 | no | 531753 | 0 | server |
| send-saturation | 1024 | `zlink-java` | 11 (low 9) | — | 0 | 4785.6 | 459 | no | 5.19 | jvm_thread_cores | 3.786 | 8 | 8 | no | 2769172 | 0 | server |
| send-saturation | 4096 | `grpc-java` | 8 | — | 0 | 4.0 | 414 | no | 2.78 | jvm_thread_cores | 0.512 | 8 | 8 | no | 159666 | 0 | server |
| send-saturation | 4096 | `zlink-framework-java` | 8 | — | 0 | 41.3 | 536 | no | 2.36 | jvm_thread_cores | 1.176 | 8 | 8 | no | 513143 | 0 | server |
| send-saturation | 4096 | `zlink-java` | 11 (low 9) | — | 0 | 681.7 | 504 | no | 4.03 | jvm_thread_cores | 2.353 | 8 | 8 | no | 1019259 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 1 | 1 | http://127.0.0.1:5240/bench/start |
| request-serial | 1024 | `zlink-framework-java` | 1 | 1 | http://127.0.0.1:5252/bench/start |
| request-serial | 1024 | `zlink-java` | 1 | 1 | http://127.0.0.1:5245/bench/start |
| request-serial | 4096 | `grpc-java` | 1 | 1 | http://127.0.0.1:5240/bench/start |
| request-serial | 4096 | `zlink-framework-java` | 1 | 1 | http://127.0.0.1:5252/bench/start |
| request-serial | 4096 | `zlink-java` | 1 | 1 | http://127.0.0.1:5245/bench/start |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-framework-java` | 8 | unbounded | http://127.0.0.1:5252/bench/start |
| request-backpressure | 1024 | `zlink-java` | 8 | unbounded | http://127.0.0.1:5245/bench/start |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-framework-java` | 8 | unbounded | http://127.0.0.1:5252/bench/start |
| request-backpressure | 4096 | `zlink-java` | 8 | unbounded | http://127.0.0.1:5245/bench/start |
| send-saturation | 1024 | `grpc-java` | 8 | 1 | http://127.0.0.1:5240/bench/start |
| send-saturation | 1024 | `zlink-framework-java` | 8 | 1 | http://127.0.0.1:5252/bench/start |
| send-saturation | 1024 | `zlink-java` | 8 | 1 | http://127.0.0.1:5245/bench/start |
| send-saturation | 4096 | `grpc-java` | 8 | 1 | http://127.0.0.1:5240/bench/start |
| send-saturation | 4096 | `zlink-framework-java` | 8 | 1 | http://127.0.0.1:5252/bench/start |
| send-saturation | 4096 | `zlink-java` | 8 | 1 | http://127.0.0.1:5245/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-java` | 3 | 7.0% | pass |
| request-serial | 1024 | `zlink-framework-java` | 3 | 14.8% | **fail** |
| request-serial | 1024 | `zlink-java` | 3 | 1.2% | pass |
| request-serial | 4096 | `grpc-java` | 3 | 2.1% | pass |
| request-serial | 4096 | `zlink-framework-java` | 3 | 15.0% | **fail** |
| request-serial | 4096 | `zlink-java` | 3 | 3.5% | pass |
| request-backpressure | 1024 | `zlink-c` | 10 | 7.3% | pass |
| request-backpressure | 1024 | `zlink-framework-java` | 3 | 7.6% | pass |
| request-backpressure | 1024 | `zlink-java` | 3 | 1.4% | pass |
| request-backpressure | 4096 | `zlink-c` | 10 | 38.9% | **fail** |
| request-backpressure | 4096 | `zlink-framework-java` | 3 | 13.0% | **fail** |
| request-backpressure | 4096 | `zlink-java` | 3 | 4.6% | pass |
| send-saturation | 1024 | `grpc-java` | 3 | 1.1% | pass |
| send-saturation | 1024 | `zlink-framework-java` | 3 | 1.0% | pass |
| send-saturation | 1024 | `zlink-java` | 3 | 1.3% | pass |
| send-saturation | 4096 | `grpc-java` | 3 | 1.4% | pass |
| send-saturation | 4096 | `zlink-framework-java` | 3 | 0.9% | pass |
| send-saturation | 4096 | `zlink-java` | 3 | 3.0% | pass |

## Excluded cells (FB-008, excluded from tables and judgement)

None. No cell was contaminated or hit the drain bound.

## Incomplete server-driven cells (excluded from tables and judgement)

None. Every server-driven source has target stats.

## Judgement (spec 7.2)

| Formula | Pattern | Payload | Value | Status | Verdict | Reason |
|---|---:|---:|---:|---|---|---|
| `zlink-java / zlink-c` | request-backpressure | 1024 | 1.052 | published | pass | both rows pass G5 (threshold 0.80) |
| `zlink-java / zlink-c` | request-backpressure | 4096 | (1.004) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 fails G5 at 38.9% (limit 10%) (threshold 0.80) |
| `zlink-framework-java / zlink-java` | request-serial | 1024 | (0.411) | **unsupported** | — | numerator zlink-framework-java-request-serial@1024 fails G5 at 14.8% (limit 10%) (threshold 0.60) |
| `zlink-framework-java / zlink-java` | request-serial | 4096 | (0.345) | **unsupported** | — | numerator zlink-framework-java-request-serial@4096 fails G5 at 15.0% (limit 10%) (threshold 0.60) |
| `zlink-framework-java / zlink-java` | request-backpressure | 1024 | 0.260 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-java / zlink-java` | request-backpressure | 4096 | (0.340) | **unsupported** | — | numerator zlink-framework-java-request-backpressure@4096 fails G5 at 13.0% (limit 10%) (threshold 0.60) |
| `zlink-framework-java / zlink-java` | send-saturation | 1024 | 0.192 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-java / zlink-java` | send-saturation | 4096 | 0.503 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**java: incomplete** — 4 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)

## node
## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-node               | 1024B    |        3.20 KOPS |    3.27 MB/s |     0.310 ms |     0.449 ms |     0.701 ms |       6.3% |   241.3 MB |       3.1% |    92.3 MB |
      | zlink-framework-node    | 1024B    |        5.43 KOPS |    5.56 MB/s |     0.181 ms |     0.242 ms |     0.375 ms |       5.3% |   145.0 MB |       3.9% |   138.6 MB |
      | zlink-node              | 1024B    |        9.08 KOPS |    9.30 MB/s |     0.108 ms |     0.140 ms |     0.264 ms |      4.6%* |   143.6 MB |       7.9% |    77.0 MB |
      | grpc-node               | 4096B    |        3.04 KOPS |   12.46 MB/s |     0.324 ms |     0.470 ms |     0.749 ms |       6.5% |   230.2 MB |       3.2% |    98.2 MB |
      | zlink-framework-node    | 4096B    |        5.08 KOPS |   20.81 MB/s |     0.193 ms |     0.253 ms |     0.395 ms |       5.3% |   157.4 MB |       3.8% |   149.5 MB |
      | zlink-node              | 4096B    |        8.81 KOPS |   36.10 MB/s |     0.111 ms |     0.145 ms |     0.286 ms |      4.5%* |   157.9 MB |       8.1% |    76.9 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      138.11 KOPS |  141.42 MB/s |   100.488 ms |   111.457 ms |   112.594 ms |      31.3% |    69.2 MB |       8.5% |    11.1 MB |
      | zlink-framework-node    | 1024B    |       16.49 KOPS |   16.88 MB/s |   549.749 ms |   586.234 ms |   589.729 ms |     12.9%* |   517.6 MB |       6.2% |   146.7 MB |
      | zlink-node              | 1024B    |       31.68 KOPS |   32.44 MB/s |     1.248 ms |     4.870 ms |    18.864 ms |     11.7%* |   153.8 MB |      12.4% |   100.1 MB |
      | zlink-c                 | 4096B    |      104.34 KOPS |  427.39 MB/s |     3.864 ms |     4.686 ms |     5.266 ms |      30.4% |    20.4 MB |      12.2% |    10.1 MB |
      | zlink-framework-node    | 4096B    |       19.17 KOPS |   78.54 MB/s |    17.580 ms |    21.116 ms |    29.053 ms |     10.9%* |   176.4 MB |       7.5% |   170.4 MB |
      | zlink-node              | 4096B    |       27.10 KOPS |  111.01 MB/s |     0.983 ms |     4.949 ms |     8.314 ms |     12.4%* |   172.5 MB |      11.8% |   106.7 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-node               | 1024B    |      5.77 KMSG/s |    5.91 MB/s |     0.624 ms |     0.967 ms |     3.099 ms |       6.8% |   243.3 MB |       2.7% |   128.6 MB |
      | zlink-framework-node    | 1024B    |     76.57 KMSG/s |   78.40 MB/s |   112.518 ms |   140.116 ms |   146.287 ms |       9.5% |   173.8 MB |       7.5% |   161.5 MB |
      | zlink-node              | 1024B    |    117.79 KMSG/s |  120.61 MB/s |    64.752 ms |    86.204 ms |    91.252 ms |     10.9%* |   172.2 MB |       7.7% |   141.6 MB |
      | grpc-node               | 4096B    |      5.46 KMSG/s |   22.37 MB/s |     0.692 ms |     1.221 ms |     3.379 ms |       6.9% |   238.3 MB |       2.7% |   125.7 MB |
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
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,138108.000
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,141.423
RESULT,current,zlink-c-request-backpressure,local,1024,latency,100.488
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,111.457
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,112.594
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,31.331
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,69.221
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.485
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,11.086
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
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,104342.400
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,427.386
RESULT,current,zlink-c-request-backpressure,local,4096,latency,3.864
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,4.686
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,5.266
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,30.429
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,20.363
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,12.247
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,10.107
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
| request-serial | 1024 | `zlink-framework-node` | 5.425 | 0.181 | 0.242 | 0.375 | 5.3 | 0.84 | 145.0 | 3.9 | 138.6 | 342 |
| request-serial | 1024 | `zlink-node` | 9.081 | 0.108 | 0.140 | 0.264 | 4.6* | 0.73 | 143.6 | 7.9 | 77.0 | 347 |
| request-serial | 4096 | `grpc-node` | 3.041 | 0.324 | 0.470 | 0.749 | 6.5 | 1.03 | 230.2 | 3.2 | 98.2 | 330 |
| request-serial | 4096 | `zlink-framework-node` | 5.080 | 0.193 | 0.253 | 0.395 | 5.3 | 0.84 | 157.4 | 3.8 | 149.5 | 340 |
| request-serial | 4096 | `zlink-node` | 8.812 | 0.111 | 0.145 | 0.286 | 4.5* | 0.72 | 157.9 | 8.1 | 76.9 | 352 |
| request-backpressure | 1024 | `zlink-c` | 138.108 | 100.488 | 111.457 | 112.594 | 31.3 | n/a | 69.2 | 8.5 | 11.1 | 17 |
| request-backpressure | 1024 | `zlink-framework-node` | 16.486 | 549.749 | 586.234 | 589.729 | 12.9* | 2.07 | 517.6 | 6.2 | 146.7 | 378 |
| request-backpressure | 1024 | `zlink-node` | 31.684 | 1.248 | 4.870 | 18.864 | 11.7* | 1.88 | 153.8 | 12.4 | 100.1 | 412 |
| request-backpressure | 4096 | `zlink-c` | 104.342 | 3.864 | 4.686 | 5.266 | 30.4 | n/a | 20.4 | 12.2 | 10.1 | 0 |
| request-backpressure | 4096 | `zlink-framework-node` | 19.174 | 17.580 | 21.116 | 29.053 | 10.9* | 1.75 | 176.4 | 7.5 | 170.4 | 381 |
| request-backpressure | 4096 | `zlink-node` | 27.102 | 0.983 | 4.949 | 8.314 | 12.4* | 1.98 | 172.5 | 11.8 | 106.7 | 405 |
| send-saturation | 1024 | `grpc-node` | 5.774 | 0.624 | 0.967 | 3.099 | 6.8 | 1.09 | 243.3 | 2.7 | 128.6 | 337 |
| send-saturation | 1024 | `zlink-framework-node` | 76.566 | 112.518 | 140.116 | 146.287 | 9.5 | 1.52 | 173.8 | 7.5 | 161.5 | 496 |
| send-saturation | 1024 | `zlink-node` | 117.787 | 64.752 | 86.204 | 91.252 | 10.9* | 1.74 | 172.2 | 7.7 | 141.6 | 557 |
| send-saturation | 4096 | `grpc-node` | 5.461 | 0.692 | 1.221 | 3.379 | 6.9 | 1.11 | 238.3 | 2.7 | 125.7 | 334 |
| send-saturation | 4096 | `zlink-framework-node` | 54.986 | 46.477 | 59.666 | 66.090 | 9.6 | 1.54 | 186.5 | 8.4 | 190.8 | 445 |
| send-saturation | 4096 | `zlink-node` | 87.574 | 2.463 | 11.375 | 18.502 | 12.8* | 2.05 | 181.9 | 11.1 | 156.3 | 581 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | execution-model ceiling | G6 status | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 1 | — | 0 | 1.0 | 331 | no | 1.01 | event_loop_utilization | 0.731 | 1 | 1 | no | 15979 | 0 | — |
| request-serial | 1024 | `zlink-framework-node` | 1 | — | 0 | 1.0 | 342 | no | 0.84 | event_loop_utilization | 0.502 | 1 | 1 | no | 27128 | 0 | — |
| request-serial | 1024 | `zlink-node` | 1 | — | 0 | 1.0 | 347 | no | 0.73 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 45408 | 0 | — |
| request-serial | 4096 | `grpc-node` | 1 | — | 0 | 1.0 | 330 | no | 1.03 | event_loop_utilization | 0.733 | 1 | 1 | no | 15208 | 0 | — |
| request-serial | 4096 | `zlink-framework-node` | 1 | — | 0 | 1.0 | 340 | no | 0.84 | event_loop_utilization | 0.481 | 1 | 1 | no | 25401 | 0 | — |
| request-serial | 4096 | `zlink-node` | 1 | — | 0 | 1.0 | 352 | no | 0.72 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 44063 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 16293 (low 15701) | — | 0 | 13878.2 | 17 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 1024 | `zlink-framework-node` | 10131 | — | 0 | 9063.4 | 378 | no | 2.07 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 92555 | 0 | — |
| request-backpressure | 1024 | `zlink-node` | 1211 (low 957) | — | 0 | 39.5 | 412 | no | 1.88 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 158424 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 1189 (low 885) | — | 0 | 403.2 | 0 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 0 | — |
| request-backpressure | 4096 | `zlink-framework-node` | 380 | — | 0 | 337.1 | 381 | no | 1.75 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 96150 | 0 | — |
| request-backpressure | 4096 | `zlink-node` | 408 (low 304) | — | 0 | 26.7 | 405 | no | 1.98 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 135512 | 0 | — |
| send-saturation | 1024 | `grpc-node` | 8 | — | 0 | 3.6 | 337 | no | 1.09 | event_loop_utilization | 0.763 | 1 | 1 | no | 28872 | 0 | server |
| send-saturation | 1024 | `zlink-framework-node` | 8 | — | 0 | 8615.1 | 496 | no | 1.52 | event_loop_utilization | 0.940 | 1 | 1 | no | 382830 | 0 | server |
| send-saturation | 1024 | `zlink-node` | 8 | — | 0 | 7626.9 | 557 | no | 1.74 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 588935 | 0 | server |
| send-saturation | 4096 | `grpc-node` | 8 | — | 0 | 3.8 | 334 | no | 1.11 | event_loop_utilization | 0.767 | 1 | 1 | no | 27304 | 0 | server |
| send-saturation | 4096 | `zlink-framework-node` | 8 | — | 0 | 2555.6 | 445 | no | 1.54 | event_loop_utilization | 0.919 | 1 | 1 | no | 274932 | 0 | server |
| send-saturation | 4096 | `zlink-node` | 8 | — | 0 | 215.7 | 581 | no | 2.05 | event_loop_utilization | 1.000 | 1 | 1 | 실행 모델 상한 도달 | 437872 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 1 | 1 | http://127.0.0.1:5220/bench/start |
| request-serial | 1024 | `zlink-framework-node` | 1 | 1 | http://127.0.0.1:5232/bench/start |
| request-serial | 1024 | `zlink-node` | 1 | 1 | http://127.0.0.1:5225/bench/start |
| request-serial | 4096 | `grpc-node` | 1 | 1 | http://127.0.0.1:5220/bench/start |
| request-serial | 4096 | `zlink-framework-node` | 1 | 1 | http://127.0.0.1:5232/bench/start |
| request-serial | 4096 | `zlink-node` | 1 | 1 | http://127.0.0.1:5225/bench/start |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-framework-node` | 8 | unbounded | http://127.0.0.1:5232/bench/start |
| request-backpressure | 1024 | `zlink-node` | 8 | unbounded | http://127.0.0.1:5225/bench/start |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-framework-node` | 8 | unbounded | http://127.0.0.1:5232/bench/start |
| request-backpressure | 4096 | `zlink-node` | 8 | unbounded | http://127.0.0.1:5225/bench/start |
| send-saturation | 1024 | `grpc-node` | 8 | 1 | http://127.0.0.1:5220/bench/start |
| send-saturation | 1024 | `zlink-framework-node` | 8 | 1 | http://127.0.0.1:5232/bench/start |
| send-saturation | 1024 | `zlink-node` | 8 | 1 | http://127.0.0.1:5225/bench/start |
| send-saturation | 4096 | `grpc-node` | 8 | 1 | http://127.0.0.1:5220/bench/start |
| send-saturation | 4096 | `zlink-framework-node` | 8 | 1 | http://127.0.0.1:5232/bench/start |
| send-saturation | 4096 | `zlink-node` | 8 | 1 | http://127.0.0.1:5225/bench/start |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 3 | 1.0% | pass |
| request-serial | 1024 | `zlink-framework-node` | 3 | 4.4% | pass |
| request-serial | 1024 | `zlink-node` | 3 | 3.7% | pass |
| request-serial | 4096 | `grpc-node` | 3 | 0.9% | pass |
| request-serial | 4096 | `zlink-framework-node` | 3 | 4.6% | pass |
| request-serial | 4096 | `zlink-node` | 3 | 6.9% | pass |
| request-backpressure | 1024 | `zlink-c` | 10 | 7.3% | pass |
| request-backpressure | 1024 | `zlink-framework-node` | 3 | 3.1% | pass |
| request-backpressure | 1024 | `zlink-node` | 3 | 4.0% | pass |
| request-backpressure | 4096 | `zlink-c` | 10 | 38.9% | **fail** |
| request-backpressure | 4096 | `zlink-framework-node` | 3 | 11.9% | **fail** |
| request-backpressure | 4096 | `zlink-node` | 3 | 12.2% | **fail** |
| send-saturation | 1024 | `grpc-node` | 3 | 2.4% | pass |
| send-saturation | 1024 | `zlink-framework-node` | 3 | 2.1% | pass |
| send-saturation | 1024 | `zlink-node` | 3 | 3.3% | pass |
| send-saturation | 4096 | `grpc-node` | 3 | 0.3% | pass |
| send-saturation | 4096 | `zlink-framework-node` | 3 | 6.7% | pass |
| send-saturation | 4096 | `zlink-node` | 3 | 7.0% | pass |

## Excluded cells (FB-008, excluded from tables and judgement)

None. No cell was contaminated or hit the drain bound.

## Incomplete server-driven cells (excluded from tables and judgement)

None. Every server-driven source has target stats.

## Judgement (spec 7.2)

| Formula | Pattern | Payload | Value | Status | Verdict | Reason |
|---|---:|---:|---:|---|---|---|
| `zlink-node / zlink-c` | request-backpressure | 1024 | 0.229 | published | **fail** | both rows pass G5 (threshold 0.80) |
| `zlink-node / zlink-c` | request-backpressure | 4096 | (0.260) | **unsupported** | — | numerator zlink-node-request-backpressure@4096 fails G5 at 12.2% (limit 10%); denominator zlink-c-request-backpressure@4096 fails G5 at 38.9% (limit 10%) (threshold 0.80) |
| `zlink-framework-node / zlink-node` | request-serial | 1024 | 0.597 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | request-serial | 4096 | 0.576 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | request-backpressure | 1024 | 0.520 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | request-backpressure | 4096 | (0.707) | **unsupported** | — | numerator zlink-framework-node-request-backpressure@4096 fails G5 at 11.9% (limit 10%); denominator zlink-node-request-backpressure@4096 fails G5 at 12.2% (limit 10%) (threshold 0.60) |
| `zlink-framework-node / zlink-node` | send-saturation | 1024 | 0.650 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-node / zlink-node` | send-saturation | 4096 | 0.628 | published | pass | both rows pass G5 (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**node: incomplete** — 2 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 2 file(s)
- run2: read cell data from 2 file(s)
- run3: read cell data from 2 file(s)
- run4: read cell data from 2 file(s)
- run5: read cell data from 2 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)


---
# #4190 재측정 및 #4162 A/A

측정 commit: `d3fd955131c1942dc46203fde365f1c690b3d0f6` (#4245 merge). 기존 측정 설정은 warmup 2초, active 5초, judged run 3회, 8 submit stream, payload 1 KiB·4 KiB다. 이번 C backpressure 보충은 두 크기별 10회 실행했다. 집계는 #4230 aggregator, `--min-runs 3` 기준이다.

## 처리량 및 기준 판정

처리량은 run 중앙값(msg/s)이다. 각 행의 기준 열은 aggregator 출력 그대로이며 `공개/통과`, `공개/실패`, `미판정/—`와 G5/G6 사유를 함께 적었다. 근거는 aggregate JSON의 구현별 행 및 judgement 행이다.

### C++
언어 판정: **미완료** — 2/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 7,704 | 13,743 | 8,822 | 0.642 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:28–72; r4190n/raw/aggregates/cpp.json:73–117; r4190n/raw/aggregates/cpp.json:118–162; r4190n/raw/aggregates/cpp.json:862–871 |
| request-serial | 4096 | 7,348 | 12,873 | 7,992 | 0.621 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:163–207; r4190n/raw/aggregates/cpp.json:208–252; r4190n/raw/aggregates/cpp.json:253–297; r4190n/raw/aggregates/cpp.json:872–881 |
| request-backpressure | 1024 | n/a | 151,793 | 47,540 | 0.313 · 공개/실패 · 양쪽 G5 통과 | 1.099 · 공개/통과 · 양쪽 G5 통과 | r4190n/raw/aggregates/cpp.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/cpp.json:343–387; r4190n/raw/aggregates/cpp.json:388–432; r4190n/raw/aggregates/cpp.json:882–891; r4190n/raw/aggregates/cpp.json:842–851 |
| request-backpressure | 4096 | n/a | 141,454 | 45,806 | 0.324 · 공개/실패 · 양쪽 G5 통과 | 1.356 · 미판정/— · 분모 G5 변동 38.9%(한도 10%) 초과 | r4190n/raw/aggregates/cpp.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/cpp.json:478–522; r4190n/raw/aggregates/cpp.json:523–567; r4190n/raw/aggregates/cpp.json:892–901; r4190n/raw/aggregates/cpp.json:852–861 |
| send-saturation | 1024 | 24,393 | 592,105 | 112,939 | 0.191 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:568–612; r4190n/raw/aggregates/cpp.json:613–657; r4190n/raw/aggregates/cpp.json:658–702; r4190n/raw/aggregates/cpp.json:902–911 |
| send-saturation | 4096 | 23,987 | 255,994 | 110,435 | 0.431 · 미판정/— · 분모 G5 변동 44.4%(한도 10%) 초과 | — (해당 기준 미적용) | r4190n/raw/aggregates/cpp.json:703–747; r4190n/raw/aggregates/cpp.json:748–792; r4190n/raw/aggregates/cpp.json:793–837; r4190n/raw/aggregates/cpp.json:912–921 |

#### C++ CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 49.7/94.2 | 38.7/35.9 | 59.9/70.4 | 58.4/45.8 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:4, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:58, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:112, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 51.6/96.4 | 39.5/37.9 | 68.1/73.8 | 61.8/49.1 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:13, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:67, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:121, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 12.5/11.8 | 62.8/56.6 | 51.5/13.5 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:40, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:94, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 20.4/18.7 | 71.7/60.5 | 48.4/23.3 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:49, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:103, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 46.4/89.4 | 1.9/2.8 | 9.1/16.9 | 9.8/3.7 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:22, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:76, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:130, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 48.0/91.2 | 3.1/4.8 | 9.1/18.4 | 14.4/8.8 | r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:31, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:85, r4190m/raw/bench/cpp-rerun1/cpu-per-cell.json:139, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

### .NET
언어 판정: **미완료** — 1/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 7,261 | 10,186 | 6,153 | 0.604 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:28–72; r4190n/raw/aggregates/dotnet.json:73–117; r4190n/raw/aggregates/dotnet.json:118–162; r4190n/raw/aggregates/dotnet.json:862–871 |
| request-serial | 4096 | 6,340 | 9,465 | 5,943 | 0.628 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:163–207; r4190n/raw/aggregates/dotnet.json:208–252; r4190n/raw/aggregates/dotnet.json:253–297; r4190n/raw/aggregates/dotnet.json:872–881 |
| request-backpressure | 1024 | n/a | 105,162 | 47,116 | 0.448 · 공개/실패 · 양쪽 G5 통과 | 0.761 · 공개/실패 · 양쪽 G5 통과 | r4190n/raw/aggregates/dotnet.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/dotnet.json:343–387; r4190n/raw/aggregates/dotnet.json:388–432; r4190n/raw/aggregates/dotnet.json:882–891; r4190n/raw/aggregates/dotnet.json:842–851 |
| request-backpressure | 4096 | n/a | 97,997 | 38,287 | 0.391 · 공개/실패 · 양쪽 G5 통과 | 0.939 · 미판정/— · 분모 G5 변동 38.9%(한도 10%) 초과 | r4190n/raw/aggregates/dotnet.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/dotnet.json:478–522; r4190n/raw/aggregates/dotnet.json:523–567; r4190n/raw/aggregates/dotnet.json:892–901; r4190n/raw/aggregates/dotnet.json:852–861 |
| send-saturation | 1024 | 23,231 | 436,668 | 194,202 | 0.445 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:568–612; r4190n/raw/aggregates/dotnet.json:613–657; r4190n/raw/aggregates/dotnet.json:658–702; r4190n/raw/aggregates/dotnet.json:902–911 |
| send-saturation | 4096 | 21,597 | 181,435 | 136,051 | 0.750 · 공개/통과 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/dotnet.json:703–747; r4190n/raw/aggregates/dotnet.json:748–792; r4190n/raw/aggregates/dotnet.json:793–837; r4190n/raw/aggregates/dotnet.json:912–921 |

#### .NET CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 436.0/341.3 | 197.5/46.2 | 263.3/91.3 | 58.4/45.8 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:4, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:58, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:112, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 448.6/359.6 | 216.0/44.5 | 272.0/90.7 | 61.8/49.1 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:13, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:67, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:121, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 31.0/11.6 | 81.2/46.3 | 51.5/13.5 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:40, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:94, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 33.4/19.1 | 100.3/53.7 | 48.4/23.3 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:49, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:103, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 240.0/244.0 | 3.9/4.0 | 5.5/14.0 | 9.8/3.7 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:22, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:76, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:130, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 252.3/265.3 | 10.3/9.9 | 10.6/20.7 | 14.4/8.8 | r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:31, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:85, r4190m/raw/bench/dotnet-cpu-rerun/cpu-per-cell.json:139, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

### Java
언어 판정: **미완료** — 4/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 8,341 | 13,737 | 5,641 | 0.411 · 미판정/— · 분자 G5 변동 14.8%(한도 10%) 초과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:28–72; r4190n/raw/aggregates/java.json:118–162; r4190n/raw/aggregates/java.json:73–117; r4190n/raw/aggregates/java.json:862–871 |
| request-serial | 4096 | 7,958 | 13,151 | 4,535 | 0.345 · 미판정/— · 분자 G5 변동 15.0%(한도 10%) 초과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:163–207; r4190n/raw/aggregates/java.json:253–297; r4190n/raw/aggregates/java.json:208–252; r4190n/raw/aggregates/java.json:872–881 |
| request-backpressure | 1024 | n/a | 145,331 | 37,719 | 0.260 · 공개/실패 · 양쪽 G5 통과 | 1.052 · 공개/통과 · 양쪽 G5 통과 | r4190n/raw/aggregates/java.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/java.json:388–432; r4190n/raw/aggregates/java.json:343–387; r4190n/raw/aggregates/java.json:882–891; r4190n/raw/aggregates/java.json:842–851 |
| request-backpressure | 4096 | n/a | 104,767 | 35,645 | 0.340 · 미판정/— · 분자 G5 변동 13.0%(한도 10%) 초과 | 1.004 · 미판정/— · 분모 G5 변동 38.9%(한도 10%) 초과 | r4190n/raw/aggregates/java.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/java.json:523–567; r4190n/raw/aggregates/java.json:478–522; r4190n/raw/aggregates/java.json:892–901; r4190n/raw/aggregates/java.json:852–861 |
| send-saturation | 1024 | 32,915 | 553,834 | 106,351 | 0.192 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:568–612; r4190n/raw/aggregates/java.json:658–702; r4190n/raw/aggregates/java.json:613–657; r4190n/raw/aggregates/java.json:902–911 |
| send-saturation | 4096 | 31,933 | 203,852 | 102,629 | 0.503 · 공개/실패 · 양쪽 G5 통과 | — (해당 기준 미적용) | r4190n/raw/aggregates/java.json:703–747; r4190n/raw/aggregates/java.json:793–837; r4190n/raw/aggregates/java.json:748–792; r4190n/raw/aggregates/java.json:912–921 |

#### Java CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 111.0/110.2 | 33.7/42.7 | 198.6/196.2 | 58.4/45.8 | r4190m/raw/bench/java/cpu-per-cell.json:4, r4190m/raw/bench/java/cpu-per-cell.json:112, r4190m/raw/bench/java/cpu-per-cell.json:58, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 111.7/114.0 | 35.7/44.8 | 198.7/209.4 | 61.8/49.1 | r4190m/raw/bench/java/cpu-per-cell.json:13, r4190m/raw/bench/java/cpu-per-cell.json:121, r4190m/raw/bench/java/cpu-per-cell.json:67, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 10.2/15.5 | 81.0/91.2 | 51.5/13.5 | r4190m/raw/bench/java/cpu-per-cell.json:94, r4190m/raw/bench/java/cpu-per-cell.json:40, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 17.0/24.1 | 85.9/96.7 | 48.4/23.3 | r4190m/raw/bench/java/cpu-per-cell.json:103, r4190m/raw/bench/java/cpu-per-cell.json:49, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 57.6/61.9 | 2.3/3.4 | 10.2/24.9 | 9.8/3.7 | r4190m/raw/bench/java/cpu-per-cell.json:22, r4190m/raw/bench/java/cpu-per-cell.json:130, r4190m/raw/bench/java/cpu-per-cell.json:76, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 59.5/64.0 | 7.6/9.4 | 11.1/27.3 | 14.4/8.8 | r4190m/raw/bench/java/cpu-per-cell.json:31, r4190m/raw/bench/java/cpu-per-cell.json:139, r4190m/raw/bench/java/cpu-per-cell.json:85, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

### Node
언어 판정: **미완료** — 8/8 judgement가 미판정이다.

| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) | 근거 (행 · 판정) |
|---|---:|---:|---:|---:|---|---|---|
| request-serial | 1024 | 3,196 | 9,081 | 5,425 | 0.597 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:28–72; r4190n/raw/aggregates/node.json:118–162; r4190n/raw/aggregates/node.json:73–117; r4190n/raw/aggregates/node.json:862–871 |
| request-serial | 4096 | 3,041 | 8,812 | 5,080 | 0.576 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:163–207; r4190n/raw/aggregates/node.json:253–297; r4190n/raw/aggregates/node.json:208–252; r4190n/raw/aggregates/node.json:872–881 |
| request-backpressure | 1024 | n/a | 31,684 | 16,486 | 0.520 · 미판정/— · 분자·분모 클라이언트 포화(G6) | 0.229 · 미판정/— · 분자 클라이언트 포화(G6) | r4190n/raw/aggregates/node.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/node.json:388–432; r4190n/raw/aggregates/node.json:343–387; r4190n/raw/aggregates/node.json:882–891; r4190n/raw/aggregates/node.json:842–851 |
| request-backpressure | 4096 | n/a | 27,102 | 19,174 | 0.707 · 미판정/— · 분자·분모 G5 변동 11.9/12.2%(한도 10%) 초과 | 0.260 · 미판정/— · 분자·분모 G5 변동 12.2/38.9%(한도 10%) 초과 | r4190n/raw/aggregates/node.md:13-17 (gRPC 역압 제외); r4190n/raw/aggregates/node.json:523–567; r4190n/raw/aggregates/node.json:478–522; r4190n/raw/aggregates/node.json:892–901; r4190n/raw/aggregates/node.json:852–861 |
| send-saturation | 1024 | 5,774 | 117,787 | 76,566 | 0.650 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:568–612; r4190n/raw/aggregates/node.json:658–702; r4190n/raw/aggregates/node.json:613–657; r4190n/raw/aggregates/node.json:902–911 |
| send-saturation | 4096 | 5,461 | 87,574 | 54,986 | 0.628 · 미판정/— · 분모 클라이언트 포화(G6) | — (해당 기준 미적용) | r4190n/raw/aggregates/node.json:703–747; r4190n/raw/aggregates/node.json:793–837; r4190n/raw/aggregates/node.json:748–792; r4190n/raw/aggregates/node.json:912–921 |

#### Node CPU
source/target CPU µs/msg. run TID union의 CPU tick 차이를 완료 메시지 수로 나눈 반복 중앙값이다. gRPC 역압은 n/a다.

| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target | CPU raw 근거 |
|---|---:|---:|---:|---:|---:|---|
| request-serial | 1024 | 318.8/157.2 | 81.5/140.5 | 162.7/116.8 | 58.4/45.8 | r4190m/raw/bench/node/cpu-per-cell.json:4, r4190m/raw/bench/node/cpu-per-cell.json:112, r4190m/raw/bench/node/cpu-per-cell.json:58, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:32 |
| request-serial | 4096 | 345.2/167.7 | 83.3/147.7 | 159.2/120.1 | 61.8/49.1 | r4190m/raw/bench/node/cpu-per-cell.json:13, r4190m/raw/bench/node/cpu-per-cell.json:121, r4190m/raw/bench/node/cpu-per-cell.json:67, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:46 |
| request-backpressure | 1024 | n/a | 59.7/63.2 | 116.1/52.8 | 51.5/13.5 | r4190m/raw/bench/node/cpu-per-cell.json:94, r4190m/raw/bench/node/cpu-per-cell.json:40, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:4 |
| request-backpressure | 4096 | n/a | 65.8/70.9 | 89.1/69.5 | 48.4/23.3 | r4190m/raw/bench/node/cpu-per-cell.json:103, r4190m/raw/bench/node/cpu-per-cell.json:49, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:18 |
| send-saturation | 1024 | 190.5/75.9 | 14.7/10.7 | 19.9/15.7 | 9.8/3.7 | r4190m/raw/bench/node/cpu-per-cell.json:22, r4190m/raw/bench/node/cpu-per-cell.json:130, r4190m/raw/bench/node/cpu-per-cell.json:76, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:60 |
| send-saturation | 4096 | 207.2/79.5 | 32.5/22.0 | 30.2/25.4 | 14.4/8.8 | r4190m/raw/bench/node/cpu-per-cell.json:31, r4190m/raw/bench/node/cpu-per-cell.json:139, r4190m/raw/bench/node/cpu-per-cell.json:85, r4190m/raw/bench/c-cpu-supplement3/cpu-per-cell.json:74 |

## C 기준 재측정 및 이전 누락 원인

r4190m의 1 KiB C run은 오류 0인 run1 한 개뿐이었다(127,492 msg/s, peak in-flight 15,783; `r4190m/raw/bench/c/run1/zlink-c-request-backpressure-1024/results.json:16,27,34`). run2·run3에는 각각 14,031·13,982 오류가 있었다(`r4190m/raw/bench/c/run2/zlink-c-request-backpressure-1024/results.json:34`, `r4190m/raw/bench/c/run3/zlink-c-request-backpressure-1024/results.json:34`). 4 KiB는 run1·run2 두 개가 오류 0(각 67,698·13,661 msg/s, peak 922·770), run3에는 205 오류가 있었다(`r4190m/raw/bench/c/run1/zlink-c-request-backpressure-4096/results.json:16,27,34`; `r4190m/raw/bench/c/run2/zlink-c-request-backpressure-4096/results.json:16,27,34`; `r4190m/raw/bench/c/run3/zlink-c-request-backpressure-4096/results.json:34`). 따라서 집계 유효 run 수는 1/2로 G5 최소 3회를 채우지 못해 B/C가 미판정이었다. 기존 raw는 처리량·오류·in-flight를 기록하지만 변동 원인은 설명하지 않는다.

새 보충 실행 10회/크기에서 오류는 모두 0이었다. 1 KiB 중앙값 138,108 msg/s, G5 변동 7.3%로 통과했다. 4 KiB 중앙값 104,342 msg/s지만 G5 변동 38.9%로 실패했다. 두 번의 보충 묶음 모두 4 KiB raw에 약 68–71k와 137–145k msg/s 두 군집이 있고, run logs는 `route ready=true`와 throughput/error만 남긴다. conditions 파일에는 uptime/load·메모리·주파수만 있고 (`r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-4096/conditions-before.txt:1-8`), source log도 route 준비·처리량·오류만 남긴다 (`r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-4096/source.log:1-2`). 따라서 그 차이의 원인은 raw에서 확정할 수 없다. 새 10회 중앙값과 G5 행은 `r4190n/raw/aggregates/cpp.json:299-340,434-476`이며, 4 KiB 분모가 G5를 통과하지 못해 네 언어 모두 해당 크기의 binding/C는 미판정이다.

새 보충 run별 처리량은 `r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run2/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run3/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run4/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run5/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run1/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run2/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run3/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run4/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run5/zlink-c-request-backpressure-1024/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run1/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run2/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run3/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run4/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun2/run5/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run1/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run2/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run3/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run4/zlink-c-request-backpressure-4096/results.json:16`, `r4190n/raw/bench/c-backpressure-rerun3/run5/zlink-c-request-backpressure-4096/results.json:16`; 오류와 peak in-flight는 같은 파일의 각 `:27,34` 행에 있다. run별 조건은 해당 셀의 `conditions-before.txt`·`conditions-after.txt`에 있다.

## Kotlin 보충 셀

요청 직렬 @1 KiB, 3회; gRPC Kotlin 및 Framework Kotlin. 처리량은 중앙값이다.

| 구현 | msg/s | source/target CPU µs/msg | raw 근거 |
|---|---:|---:|---|
| gRPC Kotlin | 6,725 | 151.6/119.3 | CPU r4190m/raw/bench/kotlin/cpu-per-cell.json:4; throughput r4190m/raw/bench/kotlin/run1/grpc-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run2/grpc-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run3/grpc-kotlin-request-serial-1024/results.json:14 |
| Framework Kotlin | 4,358 | 286.3/252.4 | CPU r4190m/raw/bench/kotlin/cpu-per-cell.json:13; throughput r4190m/raw/bench/kotlin/run1/zlink-framework-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run2/zlink-framework-kotlin-request-serial-1024/results.json:14, r4190m/raw/bench/kotlin/run3/zlink-framework-kotlin-request-serial-1024/results.json:14 |

## A/A (#4162)

20개 `aa-clean` run raw에는 단일 `latency`, `latency_p95`, `latency_p99` 값만 있다. p50 필드와 샘플별 지연값은 없다. 따라서 per-run p50, p50 중앙값/범위 및 중앙값 대비 ±5% 판정은 **자료 부족으로 미판정**이다. 예: `f4162/raw/aa-clean-01.log:5-7`, `aa-clean-20.log:5-7`; 같은 형식의 `aa-clean-{01..20}.log` 전체를 확인했다.

참고로 단일 latency 값의 중앙값은 0.006109 ms, 범위는 0.003244–0.047212 ms다. 이 값은 p50으로 볼 수 없다. p95 중앙값/범위는 0.004688 / 0.003424–0.150528 ms, p99는 0.059520 / 0.016896–1.400832 ms다. 통계 입력은 `f4162/raw/aa-clean-{01..20}.log:5-7`; 각 run별 원시 행을 보존했다.

## r4190m 대비 바뀐 것

- 모든 언어 throughput 표에 빠졌던 request-backpressure 두 크기와 해당 aggregator 판정·사유를 추가했다.
- C backpressure 10회/크기를 추가했다. 1 KiB B/C는 이제 판정 가능하고, 4 KiB는 G5 변동으로 미판정이다.
- A/A p50을 기존 analyzer 결과에서 추정하지 않았다. 원시에 p50/샘플이 없음을 확인해 미판정으로 고쳤다.

## 측정 조건 및 결과 위치

- C 재측정 lock window: 첫 묶음 13:16:12–13:17:34 UTC (`raw/c-rerun2-lock-{before,after}.txt:1`), 두 번째 묶음 13:18:56–13:20:18 UTC (`raw/c-rerun3-lock-{before,after}.txt:1`). 두 창 모두 cpu0–15 governor `performance`, 종료 시 trap으로 `powersave` 복원. placement B(source 0–7, target 8–15).
- uptime/load, free memory, cpu0–15 `scaling_cur_freq`는 매 셀 `conditions-before.txt`/`conditions-after.txt`에 저장했다. fresh C run 폴더는 `raw/bench/c-backpressure-rerun2/`, `raw/bench/c-backpressure-rerun3/`; 각 결과 JSON의 `metadata`는 run 설정을 담는다. A/A 조건 전·후는 `f4162/raw/aa-clean-conditions-before.txt`, `aa-clean-conditions-after.txt`다.
- 언어 측정과 기존 CPU supplement는 r4190m raw를 그대로 참조한다. 네 새 aggregate는 fresh C backpressure와 기존 언어 run을 함께 넣어 재생성했다: `raw/aggregates/{cpp,dotnet,java,node}.{md,json}`.
- 원시 출력은 삭제하지 않았다. C 기준용 server/client만 `cpp-build-slot.sh`를 통해 `/root/jobs/fx/cx/v21/s033/r4190n/build/c`에 빌드했고 Framework runtime은 재빌드하지 않았다.

## 남은 문제

- C request-backpressure @4 KiB G5 변동폭 38.9%로 판단 불가; 네 언어의 binding/C @4 KiB도 aggregator가 미판정 처리한다.
- A/A p50은 보존된 원시 데이터에 없어 계산·판정할 수 없다.
