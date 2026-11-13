## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |        7.27 KOPS |    7.44 MB/s |     0.126 ms |     0.168 ms |     0.198 ms |      41.0% |   100.7 MB |      30.5% |   155.7 MB |
      | zlink-c                 | 1024B    |       15.17 KOPS |   15.54 MB/s |     0.060 ms |     0.072 ms |     0.086 ms |       4.4% |     9.8 MB |       3.3% |     8.1 MB |
      | zlink-dotnet            | 1024B    |       10.20 KOPS |   10.44 MB/s |     0.090 ms |     0.112 ms |     0.136 ms |      28.7% |    97.6 MB |       5.4% |   154.2 MB |
      | zlink-framework-dotnet  | 1024B    |        6.20 KOPS |    6.35 MB/s |     0.151 ms |     0.193 ms |     0.234 ms |      23.7% |   127.2 MB |       7.1% |   190.2 MB |
      | grpc-dotnet             | 4096B    |        6.33 KOPS |   25.92 MB/s |     0.144 ms |     0.183 ms |     0.217 ms |      39.1% |   105.7 MB |      28.7% |   161.5 MB |
      | zlink-c                 | 4096B    |       14.33 KOPS |   58.68 MB/s |     0.063 ms |     0.077 ms |     0.096 ms |       4.3% |     9.6 MB |       3.3% |     8.3 MB |
      | zlink-dotnet            | 4096B    |        9.30 KOPS |   38.09 MB/s |     0.096 ms |     0.117 ms |     0.144 ms |      29.0% |    96.5 MB |       5.2% |   165.6 MB |
      | zlink-framework-dotnet  | 4096B    |        5.99 KOPS |   24.55 MB/s |     0.155 ms |     0.195 ms |     0.242 ms |      24.0% |   128.1 MB |       6.9% |   191.7 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      127.49 KOPS |  130.55 MB/s |   109.022 ms |   118.773 ms |   120.315 ms |      32.3% |    74.8 MB |       8.8% |    11.0 MB |
      | zlink-dotnet            | 1024B    |      101.77 KOPS |  104.21 MB/s |    46.929 ms |    70.011 ms |    77.505 ms |      67.0% |   166.7 MB |      14.9% |   166.8 MB |
      | zlink-framework-dotnet  | 1024B    |       46.51 KOPS |   47.63 MB/s |     5.903 ms |    18.232 ms |    25.755 ms |      75.8% |   156.7 MB |      27.2% |   205.7 MB |
      | zlink-c                 | 4096B    |       40.68 KOPS |  166.62 MB/s |     6.031 ms |     6.619 ms |     7.108 ms |      18.2% |    18.6 MB |       6.3% |     9.7 MB |
      | zlink-dotnet            | 4096B    |       97.40 KOPS |  398.94 MB/s |     3.537 ms |     5.704 ms |     6.891 ms |      69.1% |   129.5 MB |      24.1% |   171.6 MB |
      | zlink-framework-dotnet  | 4096B    |       38.26 KOPS |  156.71 MB/s |     3.429 ms |     8.564 ms |    10.564 ms |      73.8% |   153.4 MB |      25.8% |   204.2 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |     23.15 KMSG/s |   23.70 MB/s |     0.172 ms |     0.267 ms |     0.384 ms |      93.4% |   106.2 MB |      70.3% |   168.4 MB |
      | zlink-c                 | 1024B    |    527.26 KMSG/s |  539.91 MB/s |    12.843 ms |     3.640 ms |     3.903 ms |      13.9% |    11.2 MB |       5.2% |    22.2 MB |
      | zlink-dotnet            | 1024B    |    437.72 KMSG/s |  448.22 MB/s |     1.645 ms |     3.469 ms |    10.383 ms |      75.8% |   127.4 MB |      21.6% |   204.9 MB |
      | zlink-framework-dotnet  | 1024B    |    194.28 KMSG/s |  198.94 MB/s |     4.204 ms |     9.149 ms |    13.911 ms |      46.4% |   136.8 MB |      33.1% |   227.6 MB |
      | grpc-dotnet             | 4096B    |     21.50 KMSG/s |   88.08 MB/s |     0.190 ms |     0.292 ms |     0.433 ms |      91.6% |   109.4 MB |      72.2% |   176.8 MB |
      | zlink-c                 | 4096B    |    207.41 KMSG/s |  849.54 MB/s |     0.904 ms |     1.178 ms |     1.880 ms |       5.0% |    11.2 MB |       2.9% |    15.1 MB |
      | zlink-dotnet            | 4096B    |    181.03 KMSG/s |  741.50 MB/s |     1.120 ms |     1.765 ms |     4.491 ms |      63.8% |   119.8 MB |      21.9% |   196.6 MB |
      | zlink-framework-dotnet  | 4096B    |    134.25 KMSG/s |  549.89 MB/s |     3.097 ms |     7.551 ms |    14.540 ms |      55.0% |   138.1 MB |      34.8% |   227.5 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-dotnet-request-serial,local,1024,throughput,7270.000
RESULT,current,grpc-dotnet-request-serial,local,1024,bandwidth,7.444
RESULT,current,grpc-dotnet-request-serial,local,1024,latency,0.126
RESULT,current,grpc-dotnet-request-serial,local,1024,latency_p95,0.168
RESULT,current,grpc-dotnet-request-serial,local,1024,latency_p99,0.198
RESULT,current,grpc-dotnet-request-serial,local,1024,client_cpu_percent,40.950
RESULT,current,grpc-dotnet-request-serial,local,1024,client_memory_mb,100.709
RESULT,current,grpc-dotnet-request-serial,local,1024,server_cpu_percent,30.537
RESULT,current,grpc-dotnet-request-serial,local,1024,server_memory_mb,155.736
RESULT,current,zlink-c-request-serial,local,1024,throughput,15171.400
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,15.536
RESULT,current,zlink-c-request-serial,local,1024,latency,0.060
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.072
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.086
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,4.397
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,9.809
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,3.312
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,8.105
RESULT,current,zlink-dotnet-request-serial,local,1024,throughput,10196.700
RESULT,current,zlink-dotnet-request-serial,local,1024,bandwidth,10.441
RESULT,current,zlink-dotnet-request-serial,local,1024,latency,0.090
RESULT,current,zlink-dotnet-request-serial,local,1024,latency_p95,0.112
RESULT,current,zlink-dotnet-request-serial,local,1024,latency_p99,0.136
RESULT,current,zlink-dotnet-request-serial,local,1024,client_cpu_percent,28.725
RESULT,current,zlink-dotnet-request-serial,local,1024,client_memory_mb,97.574
RESULT,current,zlink-dotnet-request-serial,local,1024,server_cpu_percent,5.425
RESULT,current,zlink-dotnet-request-serial,local,1024,server_memory_mb,154.182
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,throughput,6201.200
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,bandwidth,6.350
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency,0.151
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency_p95,0.193
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency_p99,0.234
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,client_cpu_percent,23.738
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,client_memory_mb,127.232
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,server_cpu_percent,7.150
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,server_memory_mb,190.244
RESULT,current,grpc-dotnet-request-serial,local,4096,throughput,6329.000
RESULT,current,grpc-dotnet-request-serial,local,4096,bandwidth,25.924
RESULT,current,grpc-dotnet-request-serial,local,4096,latency,0.144
RESULT,current,grpc-dotnet-request-serial,local,4096,latency_p95,0.183
RESULT,current,grpc-dotnet-request-serial,local,4096,latency_p99,0.217
RESULT,current,grpc-dotnet-request-serial,local,4096,client_cpu_percent,39.062
RESULT,current,grpc-dotnet-request-serial,local,4096,client_memory_mb,105.658
RESULT,current,grpc-dotnet-request-serial,local,4096,server_cpu_percent,28.737
RESULT,current,grpc-dotnet-request-serial,local,4096,server_memory_mb,161.525
RESULT,current,zlink-c-request-serial,local,4096,throughput,14326.200
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,58.680
RESULT,current,zlink-c-request-serial,local,4096,latency,0.063
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.077
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.096
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,4.284
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,9.598
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,3.349
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.270
RESULT,current,zlink-dotnet-request-serial,local,4096,throughput,9300.400
RESULT,current,zlink-dotnet-request-serial,local,4096,bandwidth,38.094
RESULT,current,zlink-dotnet-request-serial,local,4096,latency,0.096
RESULT,current,zlink-dotnet-request-serial,local,4096,latency_p95,0.117
RESULT,current,zlink-dotnet-request-serial,local,4096,latency_p99,0.144
RESULT,current,zlink-dotnet-request-serial,local,4096,client_cpu_percent,28.962
RESULT,current,zlink-dotnet-request-serial,local,4096,client_memory_mb,96.496
RESULT,current,zlink-dotnet-request-serial,local,4096,server_cpu_percent,5.162
RESULT,current,zlink-dotnet-request-serial,local,4096,server_memory_mb,165.551
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,throughput,5993.200
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,bandwidth,24.548
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency,0.155
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency_p95,0.195
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency_p99,0.242
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,client_cpu_percent,24.013
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,client_memory_mb,128.080
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,server_cpu_percent,6.863
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,server_memory_mb,191.660
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,127491.800
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,130.552
RESULT,current,zlink-c-request-backpressure,local,1024,latency,109.022
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,118.773
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,120.315
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,32.333
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,74.762
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.823
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,10.953
RESULT,current,zlink-dotnet-request-backpressure,local,1024,throughput,101769.700
RESULT,current,zlink-dotnet-request-backpressure,local,1024,bandwidth,104.212
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency,46.929
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency_p95,70.011
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency_p99,77.505
RESULT,current,zlink-dotnet-request-backpressure,local,1024,client_cpu_percent,66.975
RESULT,current,zlink-dotnet-request-backpressure,local,1024,client_memory_mb,166.738
RESULT,current,zlink-dotnet-request-backpressure,local,1024,server_cpu_percent,14.900
RESULT,current,zlink-dotnet-request-backpressure,local,1024,server_memory_mb,166.803
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,throughput,46514.800
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,bandwidth,47.631
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency,5.903
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency_p95,18.232
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency_p99,25.755
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,client_cpu_percent,75.762
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,client_memory_mb,156.725
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,server_cpu_percent,27.150
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,server_memory_mb,205.736
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,40679.100
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,166.622
RESULT,current,zlink-c-request-backpressure,local,4096,latency,6.031
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,6.619
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,7.108
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,18.192
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,18.641
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,6.274
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,9.686
RESULT,current,zlink-dotnet-request-backpressure,local,4096,throughput,97397.100
RESULT,current,zlink-dotnet-request-backpressure,local,4096,bandwidth,398.939
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency,3.537
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency_p95,5.704
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency_p99,6.891
RESULT,current,zlink-dotnet-request-backpressure,local,4096,client_cpu_percent,69.150
RESULT,current,zlink-dotnet-request-backpressure,local,4096,client_memory_mb,129.529
RESULT,current,zlink-dotnet-request-backpressure,local,4096,server_cpu_percent,24.100
RESULT,current,zlink-dotnet-request-backpressure,local,4096,server_memory_mb,171.605
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,throughput,38259.500
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,bandwidth,156.711
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency,3.429
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency_p95,8.564
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency_p99,10.564
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,client_cpu_percent,73.775
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,client_memory_mb,153.385
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,server_cpu_percent,25.837
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,server_memory_mb,204.209
RESULT,current,grpc-dotnet-send-saturation,local,1024,throughput,23147.100
RESULT,current,grpc-dotnet-send-saturation,local,1024,bandwidth,23.703
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency,0.172
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency_p95,0.267
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency_p99,0.384
RESULT,current,grpc-dotnet-send-saturation,local,1024,client_cpu_percent,93.388
RESULT,current,grpc-dotnet-send-saturation,local,1024,client_memory_mb,106.166
RESULT,current,grpc-dotnet-send-saturation,local,1024,server_cpu_percent,70.262
RESULT,current,grpc-dotnet-send-saturation,local,1024,server_memory_mb,168.379
RESULT,current,zlink-c-send-saturation,local,1024,throughput,527255.600
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,539.910
RESULT,current,zlink-c-send-saturation,local,1024,latency,12.843
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,3.640
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,3.903
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,13.936
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,11.184
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,5.249
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,22.164
RESULT,current,zlink-dotnet-send-saturation,local,1024,throughput,437716.000
RESULT,current,zlink-dotnet-send-saturation,local,1024,bandwidth,448.221
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency,1.645
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency_p95,3.469
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency_p99,10.383
RESULT,current,zlink-dotnet-send-saturation,local,1024,client_cpu_percent,75.850
RESULT,current,zlink-dotnet-send-saturation,local,1024,client_memory_mb,127.400
RESULT,current,zlink-dotnet-send-saturation,local,1024,server_cpu_percent,21.587
RESULT,current,zlink-dotnet-send-saturation,local,1024,server_memory_mb,204.943
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,throughput,194275.300
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,bandwidth,198.938
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency,4.204
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency_p95,9.149
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency_p99,13.911
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,client_cpu_percent,46.388
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,client_memory_mb,136.773
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,server_cpu_percent,33.138
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,server_memory_mb,227.562
RESULT,current,grpc-dotnet-send-saturation,local,4096,throughput,21503.800
RESULT,current,grpc-dotnet-send-saturation,local,4096,bandwidth,88.080
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency,0.190
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency_p95,0.292
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency_p99,0.433
RESULT,current,grpc-dotnet-send-saturation,local,4096,client_cpu_percent,91.575
RESULT,current,grpc-dotnet-send-saturation,local,4096,client_memory_mb,109.434
RESULT,current,grpc-dotnet-send-saturation,local,4096,server_cpu_percent,72.175
RESULT,current,grpc-dotnet-send-saturation,local,4096,server_memory_mb,176.832
RESULT,current,zlink-c-send-saturation,local,4096,throughput,207407.800
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,849.542
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.904
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,1.178
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,1.880
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,4.980
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,11.164
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,2.870
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,15.086
RESULT,current,zlink-dotnet-send-saturation,local,4096,throughput,181030.500
RESULT,current,zlink-dotnet-send-saturation,local,4096,bandwidth,741.501
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency,1.120
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency_p95,1.765
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency_p99,4.491
RESULT,current,zlink-dotnet-send-saturation,local,4096,client_cpu_percent,63.837
RESULT,current,zlink-dotnet-send-saturation,local,4096,client_memory_mb,119.771
RESULT,current,zlink-dotnet-send-saturation,local,4096,server_cpu_percent,21.888
RESULT,current,zlink-dotnet-send-saturation,local,4096,server_memory_mb,196.604
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,throughput,134251.100
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,bandwidth,549.893
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency,3.097
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency_p95,7.551
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency_p99,14.540
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,client_cpu_percent,54.950
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,client_memory_mb,138.051
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,server_cpu_percent,34.763
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,server_memory_mb,227.537


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 7.270 | 0.126 | 0.168 | 0.198 | 41.0 | 3.28 | 100.7 | 30.5 | 155.7 | 330 |
| request-serial | 1024 | `zlink-c` | 15.171 | 0.060 | 0.072 | 0.086 | 4.4 | n/a | 9.8 | 3.3 | 8.1 | 0 |
| request-serial | 1024 | `zlink-dotnet` | 10.197 | 0.090 | 0.112 | 0.136 | 28.7 | 2.30 | 97.6 | 5.4 | 154.2 | 330 |
| request-serial | 1024 | `zlink-framework-dotnet` | 6.201 | 0.151 | 0.193 | 0.234 | 23.7 | 1.90 | 127.2 | 7.1 | 190.2 | 332 |
| request-serial | 4096 | `grpc-dotnet` | 6.329 | 0.144 | 0.183 | 0.217 | 39.1 | 3.12 | 105.7 | 28.7 | 161.5 | 328 |
| request-serial | 4096 | `zlink-c` | 14.326 | 0.063 | 0.077 | 0.096 | 4.3 | n/a | 9.6 | 3.3 | 8.3 | 0 |
| request-serial | 4096 | `zlink-dotnet` | 9.300 | 0.096 | 0.117 | 0.144 | 29.0 | 2.32 | 96.5 | 5.2 | 165.6 | 334 |
| request-serial | 4096 | `zlink-framework-dotnet` | 5.993 | 0.155 | 0.195 | 0.242 | 24.0 | 1.92 | 128.1 | 6.9 | 191.7 | 332 |
| request-backpressure | 1024 | `zlink-c` | 127.492 | 109.022 | 118.773 | 120.315 | 32.3 | n/a | 74.8 | 8.8 | 11.0 | 21 |
| request-backpressure | 1024 | `zlink-dotnet` | 101.770 | 46.929 | 70.011 | 77.505 | 67.0 | 5.36 | 166.7 | 14.9 | 166.8 | 332 |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 46.515 | 5.903 | 18.232 | 25.755 | 75.8 | 6.06 | 156.7 | 27.2 | 205.7 | 335 |
| request-backpressure | 4096 | `zlink-c` | 40.679 | 6.031 | 6.619 | 7.108 | 18.2 | n/a | 18.6 | 6.3 | 9.7 | 1 |
| request-backpressure | 4096 | `zlink-dotnet` | 97.397 | 3.537 | 5.704 | 6.891 | 69.1 | 5.53 | 129.5 | 24.1 | 171.6 | 331 |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 38.260 | 3.429 | 8.564 | 10.564 | 73.8 | 5.90 | 153.4 | 25.8 | 204.2 | 332 |
| send-saturation | 1024 | `grpc-dotnet` | 23.147 | 0.172 | 0.267 | 0.384 | 93.4 | 7.47 | 106.2 | 70.3 | 168.4 | 351 |
| send-saturation | 1024 | `zlink-c` | 527.256 | 12.843 | 3.640 | 3.903 | 13.9 | n/a | 11.2 | 5.2 | 22.2 | 2 |
| send-saturation | 1024 | `zlink-dotnet` | 437.716 | 1.645 | 3.469 | 10.383 | 75.8 | 6.07 | 127.4 | 21.6 | 204.9 | 640 |
| send-saturation | 1024 | `zlink-framework-dotnet` | 194.275 | 4.204 | 9.149 | 13.911 | 46.4 | 3.71 | 136.8 | 33.1 | 227.6 | 462 |
| send-saturation | 4096 | `grpc-dotnet` | 21.504 | 0.190 | 0.292 | 0.433 | 91.6 | 7.33 | 109.4 | 72.2 | 176.8 | 378 |
| send-saturation | 4096 | `zlink-c` | 207.408 | 0.904 | 1.178 | 1.880 | 5.0 | n/a | 11.2 | 2.9 | 15.1 | 1 |
| send-saturation | 4096 | `zlink-dotnet` | 181.030 | 1.120 | 1.765 | 4.491 | 63.8 | 5.11 | 119.8 | 21.9 | 196.6 | 466 |
| send-saturation | 4096 | `zlink-framework-dotnet` | 134.251 | 3.097 | 7.551 | 14.540 | 55.0 | 4.40 | 138.1 | 34.8 | 227.5 | 426 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | saturated | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 1 | — | 0 | 0.9 | 330 | no | 3.28 | client_cores | 3.276 | 8 | no | 36351 | 0 | — |
| request-serial | 1024 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 1024 | `zlink-dotnet` | 1 | — | 0 | 0.9 | 330 | no | 2.30 | client_cores | 2.298 | 8 | no | 50984 | 0 | — |
| request-serial | 1024 | `zlink-framework-dotnet` | 1 | — | 0 | 0.9 | 332 | no | 1.90 | client_cores | 1.899 | 8 | no | 31007 | 0 | — |
| request-serial | 4096 | `grpc-dotnet` | 1 | — | 0 | 0.9 | 328 | no | 3.12 | client_cores | 3.125 | 8 | no | 31646 | 0 | — |
| request-serial | 4096 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | not judged | n/a | 0 | — |
| request-serial | 4096 | `zlink-dotnet` | 1 | — | 0 | 0.9 | 334 | no | 2.32 | client_cores | 2.317 | 8 | no | 46503 | 0 | — |
| request-serial | 4096 | `zlink-framework-dotnet` | 1 | — | 0 | 0.9 | 332 | no | 1.92 | client_cores | 1.921 | 8 | no | 29967 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 15783 | — | 0 | 13899.5 | 21 | no | n/a | client_cores | n/a | — | not judged | n/a | 14031 | — |
| request-backpressure | 1024 | `zlink-dotnet` | 8505 (low 7220) | — | 0 | 4776.0 | 332 | no | 5.36 | client_cores | 5.358 | 8 | no | 514142 | 0 | — |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 2416 (low 1920) | — | 0 | 274.6 | 335 | no | 6.06 | client_cores | 6.061 | 8 | no | 233105 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 922 (low 770) | — | 0 | 245.3 | 1 | no | n/a | client_cores | n/a | — | not judged | n/a | 205 | — |
| request-backpressure | 4096 | `zlink-dotnet` | 1578 (low 1326) | — | 0 | 344.5 | 331 | no | 5.53 | client_cores | 5.532 | 8 | no | 487342 | 0 | — |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 794 (low 707) | — | 0 | 131.2 | 332 | no | 5.90 | client_cores | 5.902 | 8 | no | 191560 | 0 | — |
| send-saturation | 1024 | `grpc-dotnet` | 8 | — | 0 | 4.0 | 351 | no | 7.47 | client_cores | 7.471 | 8 | no | 115736 | 0 | server |
| send-saturation | 1024 | `zlink-c` | 8 | — | 0 | 6771.5 | 2 | no | n/a | client_cores | n/a | — | not judged | 2636278 | 0 | server |
| send-saturation | 1024 | `zlink-dotnet` | 8 | — | 0 | 719.9 | 640 | no | 6.07 | client_cores | 6.068 | 8 | no | 2188580 | 0 | server |
| send-saturation | 1024 | `zlink-framework-dotnet` | 8 | — | 0 | 816.7 | 462 | no | 3.71 | client_cores | 3.711 | 8 | no | 971376 | 0 | server |
| send-saturation | 4096 | `grpc-dotnet` | 8 | — | 0 | 4.1 | 378 | no | 7.33 | client_cores | 7.326 | 8 | no | 107519 | 0 | server |
| send-saturation | 4096 | `zlink-c` | 8 | — | 0 | 187.6 | 1 | no | n/a | client_cores | n/a | — | not judged | 1037039 | 0 | server |
| send-saturation | 4096 | `zlink-dotnet` | 8 | — | 0 | 202.7 | 466 | no | 5.11 | client_cores | 5.107 | 8 | no | 905152 | 0 | server |
| send-saturation | 4096 | `zlink-framework-dotnet` | 8 | — | 0 | 415.7 | 426 | no | 4.40 | client_cores | 4.396 | 8 | no | 671256 | 0 | server |

## Server-driven companion information (spec 7.1)

| Pattern | Size | Implementation | Streams | In-flight/stream | Trigger endpoint |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 1 | 1 | http://127.0.0.1:5200 |
| request-serial | 1024 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 1024 | `zlink-dotnet` | 1 | 1 | http://127.0.0.1:5205 |
| request-serial | 1024 | `zlink-framework-dotnet` | 1 | 1 | http://127.0.0.1:5212 |
| request-serial | 4096 | `grpc-dotnet` | 1 | 1 | http://127.0.0.1:5200 |
| request-serial | 4096 | `zlink-c` | 1 | 1 | n/a |
| request-serial | 4096 | `zlink-dotnet` | 1 | 1 | http://127.0.0.1:5205 |
| request-serial | 4096 | `zlink-framework-dotnet` | 1 | 1 | http://127.0.0.1:5212 |
| request-backpressure | 1024 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 1024 | `zlink-dotnet` | 8 | unbounded | http://127.0.0.1:5205 |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 8 | unbounded | http://127.0.0.1:5212 |
| request-backpressure | 4096 | `zlink-c` | 8 | unbounded | n/a |
| request-backpressure | 4096 | `zlink-dotnet` | 8 | unbounded | http://127.0.0.1:5205 |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 8 | unbounded | http://127.0.0.1:5212 |
| send-saturation | 1024 | `grpc-dotnet` | 8 | 1 | http://127.0.0.1:5200 |
| send-saturation | 1024 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 1024 | `zlink-dotnet` | 8 | 1 | http://127.0.0.1:5205 |
| send-saturation | 1024 | `zlink-framework-dotnet` | 8 | 1 | http://127.0.0.1:5212 |
| send-saturation | 4096 | `grpc-dotnet` | 8 | 1 | http://127.0.0.1:5200 |
| send-saturation | 4096 | `zlink-c` | 8 | 1 | n/a |
| send-saturation | 4096 | `zlink-dotnet` | 8 | 1 | http://127.0.0.1:5205 |
| send-saturation | 4096 | `zlink-framework-dotnet` | 8 | 1 | http://127.0.0.1:5212 |

`n/a` means the cell files carried no `trigger.endpoint`; the aggregator does not infer one from spec 9 ports.

## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 6 | 2.3% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 7.9% | pass |
| request-serial | 1024 | `zlink-dotnet` | 6 | 7.6% | pass |
| request-serial | 1024 | `zlink-framework-dotnet` | 6 | 2.9% | pass |
| request-serial | 4096 | `grpc-dotnet` | 6 | 3.4% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 2.9% | pass |
| request-serial | 4096 | `zlink-dotnet` | 6 | 8.8% | pass |
| request-serial | 4096 | `zlink-framework-dotnet` | 6 | 2.3% | pass |
| request-backpressure | 1024 | `zlink-c` | 1 | n/a (1 run) | n/a |
| request-backpressure | 1024 | `zlink-dotnet` | 6 | 6.0% | pass |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 6 | 4.7% | pass |
| request-backpressure | 4096 | `zlink-c` | 2 | 66.4% | n/a |
| request-backpressure | 4096 | `zlink-dotnet` | 6 | 3.1% | pass |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 6 | 3.0% | pass |
| send-saturation | 1024 | `grpc-dotnet` | 6 | 3.4% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 71.1% | **fail** |
| send-saturation | 1024 | `zlink-dotnet` | 6 | 1.1% | pass |
| send-saturation | 1024 | `zlink-framework-dotnet` | 6 | 2.5% | pass |
| send-saturation | 4096 | `grpc-dotnet` | 6 | 1.9% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 64.0% | **fail** |
| send-saturation | 4096 | `zlink-dotnet` | 6 | 2.1% | pass |
| send-saturation | 4096 | `zlink-framework-dotnet` | 6 | 3.7% | pass |

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
| `zlink-dotnet / zlink-c` | request-backpressure | 1024 | (0.798) | **unsupported** | — | denominator zlink-c-request-backpressure@1024 has 1 run(s); G5 needs 3; numerator zlink-dotnet-request-backpressure@1024: warmup 2s, spec requires 30s; denominator zlink-c-request-backpressure@1024: warmup 2s, spec requires 30s (threshold 0.80) |
| `zlink-dotnet / zlink-c` | request-backpressure | 4096 | (2.394) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 has 2 run(s); G5 needs 3; numerator zlink-dotnet-request-backpressure@4096: warmup 2s, spec requires 30s; denominator zlink-c-request-backpressure@4096: warmup 2s, spec requires 30s (threshold 0.80) |
| `zlink-framework-dotnet / zlink-dotnet` | request-serial | 1024 | (0.608) | **unsupported** | — | numerator zlink-framework-dotnet-request-serial@1024: warmup 2s, spec requires 30s; denominator zlink-dotnet-request-serial@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-serial | 4096 | (0.644) | **unsupported** | — | numerator zlink-framework-dotnet-request-serial@4096: warmup 2s, spec requires 30s; denominator zlink-dotnet-request-serial@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-backpressure | 1024 | (0.457) | **unsupported** | — | numerator zlink-framework-dotnet-request-backpressure@1024: warmup 2s, spec requires 30s; denominator zlink-dotnet-request-backpressure@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-backpressure | 4096 | (0.393) | **unsupported** | — | numerator zlink-framework-dotnet-request-backpressure@4096: warmup 2s, spec requires 30s; denominator zlink-dotnet-request-backpressure@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | send-saturation | 1024 | (0.444) | **unsupported** | — | numerator zlink-framework-dotnet-send-saturation@1024: warmup 2s, spec requires 30s; denominator zlink-dotnet-send-saturation@1024: warmup 2s, spec requires 30s (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | send-saturation | 4096 | (0.742) | **unsupported** | — | numerator zlink-framework-dotnet-send-saturation@4096: warmup 2s, spec requires 30s; denominator zlink-dotnet-send-saturation@4096: warmup 2s, spec requires 30s (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**dotnet: incomplete** — 8 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 6 file(s)
- run2: read cell data from 6 file(s)
- run3: read cell data from 6 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)
