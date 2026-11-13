## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |        7.28 KOPS |    7.45 MB/s |     0.126 ms |     0.168 ms |     0.195 ms |      40.8% |   100.7 MB |      30.3% |   155.6 MB |
      | zlink-c                 | 1024B    |       15.17 KOPS |   15.54 MB/s |     0.060 ms |     0.072 ms |     0.086 ms |       4.4% |     9.8 MB |       3.3% |     8.1 MB |
      | zlink-dotnet            | 1024B    |       10.21 KOPS |   10.45 MB/s |     0.090 ms |     0.112 ms |     0.138 ms |      28.9% |    97.5 MB |       5.3% |   153.1 MB |
      | zlink-framework-dotnet  | 1024B    |        6.35 KOPS |    6.50 MB/s |     0.148 ms |     0.187 ms |     0.227 ms |      24.3% |   126.9 MB |       7.1% |   183.5 MB |
      | grpc-dotnet             | 4096B    |        6.32 KOPS |   25.88 MB/s |     0.144 ms |     0.190 ms |     0.232 ms |      38.9% |   106.2 MB |      28.5% |   162.1 MB |
      | zlink-c                 | 4096B    |       14.33 KOPS |   58.68 MB/s |     0.063 ms |     0.077 ms |     0.096 ms |       4.3% |     9.6 MB |       3.3% |     8.3 MB |
      | zlink-dotnet            | 4096B    |        9.27 KOPS |   37.98 MB/s |     0.096 ms |     0.117 ms |     0.144 ms |      28.8% |    95.9 MB |       5.2% |   165.1 MB |
      | zlink-framework-dotnet  | 4096B    |        6.02 KOPS |   24.65 MB/s |     0.155 ms |     0.194 ms |     0.242 ms |      24.1% |   127.7 MB |       6.8% |   191.6 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | zlink-c                 | 1024B    |      127.49 KOPS |  130.55 MB/s |   109.022 ms |   118.773 ms |   120.315 ms |      32.3% |    74.8 MB |       8.8% |    11.0 MB |
      | zlink-dotnet            | 1024B    |      100.97 KOPS |  103.39 MB/s |    48.673 ms |    72.976 ms |    81.600 ms |      67.0% |   163.8 MB |      14.9% |   167.3 MB |
      | zlink-framework-dotnet  | 1024B    |       45.91 KOPS |   47.02 MB/s |     5.809 ms |    17.767 ms |    25.856 ms |      77.5% |   156.3 MB |      27.1% |   209.6 MB |
      | zlink-c                 | 4096B    |       40.68 KOPS |  166.62 MB/s |     6.031 ms |     6.619 ms |     7.108 ms |      18.2% |    18.6 MB |       6.3% |     9.7 MB |
      | zlink-dotnet            | 4096B    |       96.80 KOPS |  396.48 MB/s |     3.477 ms |     5.727 ms |     6.892 ms |      69.5% |   129.3 MB |      24.4% |   172.1 MB |
      | zlink-framework-dotnet  | 4096B    |       37.66 KOPS |  154.25 MB/s |     3.445 ms |     8.529 ms |    10.532 ms |      73.8% |   153.3 MB |      25.4% |   204.0 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-dotnet             | 1024B    |     23.06 KMSG/s |   23.62 MB/s |     0.173 ms |     0.270 ms |     0.388 ms |      93.5% |   105.8 MB |      70.6% |   168.0 MB |
      | zlink-c                 | 1024B    |    527.26 KMSG/s |  539.91 MB/s |    12.843 ms |     3.640 ms |     3.903 ms |      13.9% |    11.2 MB |       5.2% |    22.2 MB |
      | zlink-dotnet            | 1024B    |    438.76 KMSG/s |  449.29 MB/s |     1.634 ms |     3.061 ms |    11.493 ms |      75.8% |   127.4 MB |      21.8% |   205.1 MB |
      | zlink-framework-dotnet  | 1024B    |    194.35 KMSG/s |  199.01 MB/s |     4.249 ms |     9.132 ms |    13.973 ms |      46.6% |   136.5 MB |      33.3% |   229.1 MB |
      | grpc-dotnet             | 4096B    |     21.36 KMSG/s |   87.49 MB/s |     0.191 ms |     0.294 ms |     0.437 ms |      91.3% |   109.1 MB |      72.8% |   177.0 MB |
      | zlink-c                 | 4096B    |    207.41 KMSG/s |  849.54 MB/s |     0.904 ms |     1.178 ms |     1.880 ms |       5.0% |    11.2 MB |       2.9% |    15.1 MB |
      | zlink-dotnet            | 4096B    |    177.64 KMSG/s |  727.63 MB/s |     1.149 ms |     1.851 ms |     5.291 ms |      65.0% |   119.7 MB |      21.9% |   196.4 MB |
      | zlink-framework-dotnet  | 4096B    |    132.45 KMSG/s |  542.52 MB/s |     2.758 ms |     6.724 ms |    13.522 ms |      55.0% |   137.6 MB |      34.6% |   225.4 MB |

## RESULT lines (spec 4; throughput in completions per second)

RESULT,current,grpc-dotnet-request-serial,local,1024,throughput,7278.800
RESULT,current,grpc-dotnet-request-serial,local,1024,bandwidth,7.453
RESULT,current,grpc-dotnet-request-serial,local,1024,latency,0.126
RESULT,current,grpc-dotnet-request-serial,local,1024,latency_p95,0.168
RESULT,current,grpc-dotnet-request-serial,local,1024,latency_p99,0.195
RESULT,current,grpc-dotnet-request-serial,local,1024,client_cpu_percent,40.850
RESULT,current,grpc-dotnet-request-serial,local,1024,client_memory_mb,100.672
RESULT,current,grpc-dotnet-request-serial,local,1024,server_cpu_percent,30.300
RESULT,current,grpc-dotnet-request-serial,local,1024,server_memory_mb,155.621
RESULT,current,zlink-c-request-serial,local,1024,throughput,15171.400
RESULT,current,zlink-c-request-serial,local,1024,bandwidth,15.536
RESULT,current,zlink-c-request-serial,local,1024,latency,0.060
RESULT,current,zlink-c-request-serial,local,1024,latency_p95,0.072
RESULT,current,zlink-c-request-serial,local,1024,latency_p99,0.086
RESULT,current,zlink-c-request-serial,local,1024,client_cpu_percent,4.397
RESULT,current,zlink-c-request-serial,local,1024,client_memory_mb,9.809
RESULT,current,zlink-c-request-serial,local,1024,server_cpu_percent,3.312
RESULT,current,zlink-c-request-serial,local,1024,server_memory_mb,8.105
RESULT,current,zlink-dotnet-request-serial,local,1024,throughput,10207.600
RESULT,current,zlink-dotnet-request-serial,local,1024,bandwidth,10.453
RESULT,current,zlink-dotnet-request-serial,local,1024,latency,0.090
RESULT,current,zlink-dotnet-request-serial,local,1024,latency_p95,0.112
RESULT,current,zlink-dotnet-request-serial,local,1024,latency_p99,0.138
RESULT,current,zlink-dotnet-request-serial,local,1024,client_cpu_percent,28.875
RESULT,current,zlink-dotnet-request-serial,local,1024,client_memory_mb,97.496
RESULT,current,zlink-dotnet-request-serial,local,1024,server_cpu_percent,5.275
RESULT,current,zlink-dotnet-request-serial,local,1024,server_memory_mb,153.113
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,throughput,6347.200
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,bandwidth,6.500
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency,0.148
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency_p95,0.187
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,latency_p99,0.227
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,client_cpu_percent,24.300
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,client_memory_mb,126.867
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,server_cpu_percent,7.100
RESULT,current,zlink-framework-dotnet-request-serial,local,1024,server_memory_mb,183.477
RESULT,current,grpc-dotnet-request-serial,local,4096,throughput,6318.200
RESULT,current,grpc-dotnet-request-serial,local,4096,bandwidth,25.879
RESULT,current,grpc-dotnet-request-serial,local,4096,latency,0.144
RESULT,current,grpc-dotnet-request-serial,local,4096,latency_p95,0.190
RESULT,current,grpc-dotnet-request-serial,local,4096,latency_p99,0.232
RESULT,current,grpc-dotnet-request-serial,local,4096,client_cpu_percent,38.900
RESULT,current,grpc-dotnet-request-serial,local,4096,client_memory_mb,106.168
RESULT,current,grpc-dotnet-request-serial,local,4096,server_cpu_percent,28.475
RESULT,current,grpc-dotnet-request-serial,local,4096,server_memory_mb,162.117
RESULT,current,zlink-c-request-serial,local,4096,throughput,14326.200
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,58.680
RESULT,current,zlink-c-request-serial,local,4096,latency,0.063
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,0.077
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,0.096
RESULT,current,zlink-c-request-serial,local,4096,client_cpu_percent,4.284
RESULT,current,zlink-c-request-serial,local,4096,client_memory_mb,9.598
RESULT,current,zlink-c-request-serial,local,4096,server_cpu_percent,3.349
RESULT,current,zlink-c-request-serial,local,4096,server_memory_mb,8.270
RESULT,current,zlink-dotnet-request-serial,local,4096,throughput,9272.800
RESULT,current,zlink-dotnet-request-serial,local,4096,bandwidth,37.981
RESULT,current,zlink-dotnet-request-serial,local,4096,latency,0.096
RESULT,current,zlink-dotnet-request-serial,local,4096,latency_p95,0.117
RESULT,current,zlink-dotnet-request-serial,local,4096,latency_p99,0.144
RESULT,current,zlink-dotnet-request-serial,local,4096,client_cpu_percent,28.775
RESULT,current,zlink-dotnet-request-serial,local,4096,client_memory_mb,95.914
RESULT,current,zlink-dotnet-request-serial,local,4096,server_cpu_percent,5.175
RESULT,current,zlink-dotnet-request-serial,local,4096,server_memory_mb,165.133
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,throughput,6018.000
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,bandwidth,24.650
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency,0.155
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency_p95,0.194
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,latency_p99,0.242
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,client_cpu_percent,24.050
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,client_memory_mb,127.672
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,server_cpu_percent,6.800
RESULT,current,zlink-framework-dotnet-request-serial,local,4096,server_memory_mb,191.609
RESULT,current,zlink-c-request-backpressure,local,1024,throughput,127491.800
RESULT,current,zlink-c-request-backpressure,local,1024,bandwidth,130.552
RESULT,current,zlink-c-request-backpressure,local,1024,latency,109.022
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p95,118.773
RESULT,current,zlink-c-request-backpressure,local,1024,latency_p99,120.315
RESULT,current,zlink-c-request-backpressure,local,1024,client_cpu_percent,32.333
RESULT,current,zlink-c-request-backpressure,local,1024,client_memory_mb,74.762
RESULT,current,zlink-c-request-backpressure,local,1024,server_cpu_percent,8.823
RESULT,current,zlink-c-request-backpressure,local,1024,server_memory_mb,10.953
RESULT,current,zlink-dotnet-request-backpressure,local,1024,throughput,100968.600
RESULT,current,zlink-dotnet-request-backpressure,local,1024,bandwidth,103.392
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency,48.673
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency_p95,72.976
RESULT,current,zlink-dotnet-request-backpressure,local,1024,latency_p99,81.600
RESULT,current,zlink-dotnet-request-backpressure,local,1024,client_cpu_percent,67.050
RESULT,current,zlink-dotnet-request-backpressure,local,1024,client_memory_mb,163.812
RESULT,current,zlink-dotnet-request-backpressure,local,1024,server_cpu_percent,14.875
RESULT,current,zlink-dotnet-request-backpressure,local,1024,server_memory_mb,167.324
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,throughput,45913.800
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,bandwidth,47.016
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency,5.809
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency_p95,17.767
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,latency_p99,25.856
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,client_cpu_percent,77.475
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,client_memory_mb,156.297
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,server_cpu_percent,27.050
RESULT,current,zlink-framework-dotnet-request-backpressure,local,1024,server_memory_mb,209.590
RESULT,current,zlink-c-request-backpressure,local,4096,throughput,40679.100
RESULT,current,zlink-c-request-backpressure,local,4096,bandwidth,166.622
RESULT,current,zlink-c-request-backpressure,local,4096,latency,6.031
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p95,6.619
RESULT,current,zlink-c-request-backpressure,local,4096,latency_p99,7.108
RESULT,current,zlink-c-request-backpressure,local,4096,client_cpu_percent,18.192
RESULT,current,zlink-c-request-backpressure,local,4096,client_memory_mb,18.641
RESULT,current,zlink-c-request-backpressure,local,4096,server_cpu_percent,6.274
RESULT,current,zlink-c-request-backpressure,local,4096,server_memory_mb,9.686
RESULT,current,zlink-dotnet-request-backpressure,local,4096,throughput,96797.400
RESULT,current,zlink-dotnet-request-backpressure,local,4096,bandwidth,396.482
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency,3.477
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency_p95,5.727
RESULT,current,zlink-dotnet-request-backpressure,local,4096,latency_p99,6.892
RESULT,current,zlink-dotnet-request-backpressure,local,4096,client_cpu_percent,69.525
RESULT,current,zlink-dotnet-request-backpressure,local,4096,client_memory_mb,129.316
RESULT,current,zlink-dotnet-request-backpressure,local,4096,server_cpu_percent,24.375
RESULT,current,zlink-dotnet-request-backpressure,local,4096,server_memory_mb,172.082
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,throughput,37657.600
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,bandwidth,154.246
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency,3.445
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency_p95,8.529
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,latency_p99,10.532
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,client_cpu_percent,73.850
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,client_memory_mb,153.297
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,server_cpu_percent,25.400
RESULT,current,zlink-framework-dotnet-request-backpressure,local,4096,server_memory_mb,203.973
RESULT,current,grpc-dotnet-send-saturation,local,1024,throughput,23063.000
RESULT,current,grpc-dotnet-send-saturation,local,1024,bandwidth,23.617
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency,0.173
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency_p95,0.270
RESULT,current,grpc-dotnet-send-saturation,local,1024,latency_p99,0.388
RESULT,current,grpc-dotnet-send-saturation,local,1024,client_cpu_percent,93.550
RESULT,current,grpc-dotnet-send-saturation,local,1024,client_memory_mb,105.812
RESULT,current,grpc-dotnet-send-saturation,local,1024,server_cpu_percent,70.600
RESULT,current,grpc-dotnet-send-saturation,local,1024,server_memory_mb,167.953
RESULT,current,zlink-c-send-saturation,local,1024,throughput,527255.600
RESULT,current,zlink-c-send-saturation,local,1024,bandwidth,539.910
RESULT,current,zlink-c-send-saturation,local,1024,latency,12.843
RESULT,current,zlink-c-send-saturation,local,1024,latency_p95,3.640
RESULT,current,zlink-c-send-saturation,local,1024,latency_p99,3.903
RESULT,current,zlink-c-send-saturation,local,1024,client_cpu_percent,13.936
RESULT,current,zlink-c-send-saturation,local,1024,client_memory_mb,11.184
RESULT,current,zlink-c-send-saturation,local,1024,server_cpu_percent,5.249
RESULT,current,zlink-c-send-saturation,local,1024,server_memory_mb,22.164
RESULT,current,zlink-dotnet-send-saturation,local,1024,throughput,438764.000
RESULT,current,zlink-dotnet-send-saturation,local,1024,bandwidth,449.294
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency,1.634
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency_p95,3.061
RESULT,current,zlink-dotnet-send-saturation,local,1024,latency_p99,11.493
RESULT,current,zlink-dotnet-send-saturation,local,1024,client_cpu_percent,75.825
RESULT,current,zlink-dotnet-send-saturation,local,1024,client_memory_mb,127.414
RESULT,current,zlink-dotnet-send-saturation,local,1024,server_cpu_percent,21.800
RESULT,current,zlink-dotnet-send-saturation,local,1024,server_memory_mb,205.094
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,throughput,194349.000
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,bandwidth,199.013
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency,4.249
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency_p95,9.132
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,latency_p99,13.973
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,client_cpu_percent,46.600
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,client_memory_mb,136.457
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,server_cpu_percent,33.325
RESULT,current,zlink-framework-dotnet-send-saturation,local,1024,server_memory_mb,229.078
RESULT,current,grpc-dotnet-send-saturation,local,4096,throughput,21361.000
RESULT,current,grpc-dotnet-send-saturation,local,4096,bandwidth,87.495
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency,0.191
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency_p95,0.294
RESULT,current,grpc-dotnet-send-saturation,local,4096,latency_p99,0.437
RESULT,current,grpc-dotnet-send-saturation,local,4096,client_cpu_percent,91.350
RESULT,current,grpc-dotnet-send-saturation,local,4096,client_memory_mb,109.094
RESULT,current,grpc-dotnet-send-saturation,local,4096,server_cpu_percent,72.800
RESULT,current,grpc-dotnet-send-saturation,local,4096,server_memory_mb,177.023
RESULT,current,zlink-c-send-saturation,local,4096,throughput,207407.800
RESULT,current,zlink-c-send-saturation,local,4096,bandwidth,849.542
RESULT,current,zlink-c-send-saturation,local,4096,latency,0.904
RESULT,current,zlink-c-send-saturation,local,4096,latency_p95,1.178
RESULT,current,zlink-c-send-saturation,local,4096,latency_p99,1.880
RESULT,current,zlink-c-send-saturation,local,4096,client_cpu_percent,4.980
RESULT,current,zlink-c-send-saturation,local,4096,client_memory_mb,11.164
RESULT,current,zlink-c-send-saturation,local,4096,server_cpu_percent,2.870
RESULT,current,zlink-c-send-saturation,local,4096,server_memory_mb,15.086
RESULT,current,zlink-dotnet-send-saturation,local,4096,throughput,177643.000
RESULT,current,zlink-dotnet-send-saturation,local,4096,bandwidth,727.626
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency,1.149
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency_p95,1.851
RESULT,current,zlink-dotnet-send-saturation,local,4096,latency_p99,5.291
RESULT,current,zlink-dotnet-send-saturation,local,4096,client_cpu_percent,64.975
RESULT,current,zlink-dotnet-send-saturation,local,4096,client_memory_mb,119.699
RESULT,current,zlink-dotnet-send-saturation,local,4096,server_cpu_percent,21.875
RESULT,current,zlink-dotnet-send-saturation,local,4096,server_memory_mb,196.355
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,throughput,132451.600
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,bandwidth,542.522
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency,2.758
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency_p95,6.724
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,latency_p99,13.522
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,client_cpu_percent,55.050
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,client_memory_mb,137.559
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,server_cpu_percent,34.650
RESULT,current,zlink-framework-dotnet-send-saturation,local,4096,server_memory_mb,225.359


## Medians across runs

| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU% | Source cores | Source MB | Target CPU% | Target MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 7.279 | 0.126 | 0.168 | 0.195 | 40.8 | 3.27 | 100.7 | 30.3 | 155.6 | 329 |
| request-serial | 1024 | `zlink-c` | 15.171 | 0.060 | 0.072 | 0.086 | 4.4 | n/a | 9.8 | 3.3 | 8.1 | 0 |
| request-serial | 1024 | `zlink-dotnet` | 10.208 | 0.090 | 0.112 | 0.138 | 28.9 | 2.31 | 97.5 | 5.3 | 153.1 | 334 |
| request-serial | 1024 | `zlink-framework-dotnet` | 6.347 | 0.148 | 0.187 | 0.227 | 24.3 | 1.94 | 126.9 | 7.1 | 183.5 | 331 |
| request-serial | 4096 | `grpc-dotnet` | 6.318 | 0.144 | 0.190 | 0.232 | 38.9 | 3.11 | 106.2 | 28.5 | 162.1 | 327 |
| request-serial | 4096 | `zlink-c` | 14.326 | 0.063 | 0.077 | 0.096 | 4.3 | n/a | 9.6 | 3.3 | 8.3 | 0 |
| request-serial | 4096 | `zlink-dotnet` | 9.273 | 0.096 | 0.117 | 0.144 | 28.8 | 2.30 | 95.9 | 5.2 | 165.1 | 331 |
| request-serial | 4096 | `zlink-framework-dotnet` | 6.018 | 0.155 | 0.194 | 0.242 | 24.1 | 1.92 | 127.7 | 6.8 | 191.6 | 333 |
| request-backpressure | 1024 | `zlink-c` | 127.492 | 109.022 | 118.773 | 120.315 | 32.3 | n/a | 74.8 | 8.8 | 11.0 | 21 |
| request-backpressure | 1024 | `zlink-dotnet` | 100.969 | 48.673 | 72.976 | 81.600 | 67.0 | 5.36 | 163.8 | 14.9 | 167.3 | 334 |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 45.914 | 5.809 | 17.767 | 25.856 | 77.5 | 6.20 | 156.3 | 27.1 | 209.6 | 335 |
| request-backpressure | 4096 | `zlink-c` | 40.679 | 6.031 | 6.619 | 7.108 | 18.2 | n/a | 18.6 | 6.3 | 9.7 | 1 |
| request-backpressure | 4096 | `zlink-dotnet` | 96.797 | 3.477 | 5.727 | 6.892 | 69.5 | 5.56 | 129.3 | 24.4 | 172.1 | 332 |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 37.658 | 3.445 | 8.529 | 10.532 | 73.8 | 5.91 | 153.3 | 25.4 | 204.0 | 333 |
| send-saturation | 1024 | `grpc-dotnet` | 23.063 | 0.173 | 0.270 | 0.388 | 93.5 | 7.48 | 105.8 | 70.6 | 168.0 | 350 |
| send-saturation | 1024 | `zlink-c` | 527.256 | 12.843 | 3.640 | 3.903 | 13.9 | n/a | 11.2 | 5.2 | 22.2 | 2 |
| send-saturation | 1024 | `zlink-dotnet` | 438.764 | 1.634 | 3.061 | 11.493 | 75.8 | 6.07 | 127.4 | 21.8 | 205.1 | 642 |
| send-saturation | 1024 | `zlink-framework-dotnet` | 194.349 | 4.249 | 9.132 | 13.973 | 46.6 | 3.73 | 136.5 | 33.3 | 229.1 | 468 |
| send-saturation | 4096 | `grpc-dotnet` | 21.361 | 0.191 | 0.294 | 0.437 | 91.3 | 7.31 | 109.1 | 72.8 | 177.0 | 378 |
| send-saturation | 4096 | `zlink-c` | 207.408 | 0.904 | 1.178 | 1.880 | 5.0 | n/a | 11.2 | 2.9 | 15.1 | 1 |
| send-saturation | 4096 | `zlink-dotnet` | 177.643 | 1.149 | 1.851 | 5.291 | 65.0 | 5.20 | 119.7 | 21.9 | 196.4 | 464 |
| send-saturation | 4096 | `zlink-framework-dotnet` | 132.452 | 2.758 | 6.724 | 13.522 | 55.0 | 4.40 | 137.6 | 34.6 | 225.4 | 425 |

## Diagnostics (FB-008, FB-017, G6, G8)

| Pattern | Size | Implementation | peak_in_flight | window | abandoned | depth (thr x lat) | drain ms | drain bound hit | source cores | saturation metric | reading | declared ceiling | execution-model ceiling | G6 status | target received | errors | send counted by |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-dotnet` | 1 | — | 0 | 0.9 | 329 | no | 3.27 | client_cores | 3.268 | 8 | 1 | no | 36395 | 0 | — |
| request-serial | 1024 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | 1 | not judged | n/a | 0 | — |
| request-serial | 1024 | `zlink-dotnet` | 1 | — | 0 | 0.9 | 334 | no | 2.31 | client_cores | 2.310 | 8 | 1 | no | 51039 | 0 | — |
| request-serial | 1024 | `zlink-framework-dotnet` | 1 | — | 0 | 0.9 | 331 | no | 1.94 | client_cores | 1.944 | 8 | 1 | no | 31737 | 0 | — |
| request-serial | 4096 | `grpc-dotnet` | 1 | — | 0 | 0.9 | 327 | no | 3.11 | client_cores | 3.112 | 8 | 1 | no | 31592 | 0 | — |
| request-serial | 4096 | `zlink-c` | 1 | — | 0 | 0.9 | 0 | no | n/a | client_cores | n/a | — | 1 | not judged | n/a | 0 | — |
| request-serial | 4096 | `zlink-dotnet` | 1 | — | 0 | 0.9 | 331 | no | 2.30 | client_cores | 2.302 | 8 | 1 | no | 46365 | 0 | — |
| request-serial | 4096 | `zlink-framework-dotnet` | 1 | — | 0 | 0.9 | 333 | no | 1.92 | client_cores | 1.924 | 8 | 1 | no | 30091 | 0 | — |
| request-backpressure | 1024 | `zlink-c` | 15783 | — | 0 | 13899.5 | 21 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 14031 | — |
| request-backpressure | 1024 | `zlink-dotnet` | 8505 (low 7959) | — | 0 | 4914.5 | 334 | no | 5.36 | client_cores | 5.364 | 8 | 8 | no | 509863 | 0 | — |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 2207 (low 1920) | — | 0 | 266.7 | 335 | no | 6.20 | client_cores | 6.198 | 8 | 8 | no | 230185 | 0 | — |
| request-backpressure | 4096 | `zlink-c` | 922 (low 770) | — | 0 | 245.3 | 1 | no | n/a | client_cores | n/a | — | 8 | not judged | n/a | 205 | — |
| request-backpressure | 4096 | `zlink-dotnet` | 1467 (low 1361) | — | 0 | 336.6 | 332 | no | 5.56 | client_cores | 5.562 | 8 | 8 | no | 484297 | 0 | — |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 749 (low 707) | — | 0 | 129.7 | 333 | no | 5.91 | client_cores | 5.908 | 8 | 8 | no | 188319 | 0 | — |
| send-saturation | 1024 | `grpc-dotnet` | 8 | — | 0 | 4.0 | 350 | no | 7.48 | client_cores | 7.484 | 8 | 8 | no | 115315 | 0 | server |
| send-saturation | 1024 | `zlink-c` | 8 | — | 0 | 6771.5 | 2 | no | n/a | client_cores | n/a | — | 8 | not judged | 2636278 | 0 | server |
| send-saturation | 1024 | `zlink-dotnet` | 8 | — | 0 | 717.2 | 642 | no | 6.07 | client_cores | 6.066 | 8 | 8 | no | 2193820 | 0 | server |
| send-saturation | 1024 | `zlink-framework-dotnet` | 8 | — | 0 | 825.8 | 468 | no | 3.73 | client_cores | 3.728 | 8 | 8 | no | 971745 | 0 | server |
| send-saturation | 4096 | `grpc-dotnet` | 8 | — | 0 | 4.1 | 378 | no | 7.31 | client_cores | 7.308 | 8 | 8 | no | 106805 | 0 | server |
| send-saturation | 4096 | `zlink-c` | 8 | — | 0 | 187.6 | 1 | no | n/a | client_cores | n/a | — | 8 | not judged | 1037039 | 0 | server |
| send-saturation | 4096 | `zlink-dotnet` | 8 | — | 0 | 204.0 | 464 | no | 5.20 | client_cores | 5.198 | 8 | 8 | no | 888215 | 0 | server |
| send-saturation | 4096 | `zlink-framework-dotnet` | 8 | — | 0 | 365.3 | 425 | no | 4.40 | client_cores | 4.404 | 8 | 8 | no | 662258 | 0 | server |

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
| request-serial | 1024 | `grpc-dotnet` | 3 | 2.4% | pass |
| request-serial | 1024 | `zlink-c` | 3 | 7.9% | pass |
| request-serial | 1024 | `zlink-dotnet` | 3 | 7.4% | pass |
| request-serial | 1024 | `zlink-framework-dotnet` | 3 | 3.5% | pass |
| request-serial | 4096 | `grpc-dotnet` | 3 | 2.4% | pass |
| request-serial | 4096 | `zlink-c` | 3 | 2.9% | pass |
| request-serial | 4096 | `zlink-dotnet` | 3 | 1.3% | pass |
| request-serial | 4096 | `zlink-framework-dotnet` | 3 | 1.8% | pass |
| request-backpressure | 1024 | `zlink-c` | 1 | n/a (1 run) | n/a |
| request-backpressure | 1024 | `zlink-dotnet` | 3 | 4.7% | pass |
| request-backpressure | 1024 | `zlink-framework-dotnet` | 3 | 5.0% | pass |
| request-backpressure | 4096 | `zlink-c` | 2 | 66.4% | n/a |
| request-backpressure | 4096 | `zlink-dotnet` | 3 | 2.9% | pass |
| request-backpressure | 4096 | `zlink-framework-dotnet` | 3 | 2.6% | pass |
| send-saturation | 1024 | `grpc-dotnet` | 3 | 3.7% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 71.1% | **fail** |
| send-saturation | 1024 | `zlink-dotnet` | 3 | 0.8% | pass |
| send-saturation | 1024 | `zlink-framework-dotnet` | 3 | 2.5% | pass |
| send-saturation | 4096 | `grpc-dotnet` | 3 | 1.3% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 64.0% | **fail** |
| send-saturation | 4096 | `zlink-dotnet` | 3 | 2.2% | pass |
| send-saturation | 4096 | `zlink-framework-dotnet` | 3 | 3.4% | pass |

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
| `zlink-dotnet / zlink-c` | request-backpressure | 1024 | (0.792) | **unsupported** | — | denominator zlink-c-request-backpressure@1024 has 1 run(s); G5 needs 3 (threshold 0.80) |
| `zlink-dotnet / zlink-c` | request-backpressure | 4096 | (2.380) | **unsupported** | — | denominator zlink-c-request-backpressure@4096 has 2 run(s); G5 needs 3 (threshold 0.80) |
| `zlink-framework-dotnet / zlink-dotnet` | request-serial | 1024 | 0.622 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-serial | 4096 | 0.649 | published | pass | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-backpressure | 1024 | 0.455 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | request-backpressure | 4096 | 0.389 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | send-saturation | 1024 | 0.443 | published | **fail** | both rows pass G5 (threshold 0.60) |
| `zlink-framework-dotnet / zlink-dotnet` | send-saturation | 4096 | 0.746 | published | pass | both rows pass G5 (threshold 0.60) |
| `grpc-<lang>` | request-backpressure | 1024 | n/a | n/a | n/a | gRPC does not measure request-backpressure |
| `grpc-<lang>` | request-backpressure | 4096 | n/a | n/a | n/a | gRPC does not measure request-backpressure |

**dotnet: incomplete** — 2 of 8 judgement(s) unsupported; spec 7.2 needs both payload sizes

## Notes

- run1: read cell data from 6 file(s)
- run2: read cell data from 6 file(s)
- run3: read cell data from 6 file(s)
- run1: read cell data from 16 file(s)
- run2: read cell data from 16 file(s)
- run3: read cell data from 16 file(s)
