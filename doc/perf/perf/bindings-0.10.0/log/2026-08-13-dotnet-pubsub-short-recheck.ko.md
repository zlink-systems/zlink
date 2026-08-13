# .NET PUB/SUB short 재측정

release Core `0.10.1`에서 C 후 .NET을 순차 실행했다. 공통 조건은
`MULTI_PUBSUB`, tcp, clients `100`, duration `2초`, runs `1`, I/O threads `4/4`,
balanced auto-HWM, send/receive timeout `200ms`다.

- C report: `/tmp/zlink-dotnet-pubsub-recheck-c/multi/report/perf_c_multi_linux_20260813_044833_dotnet-pubsub-recheck-c.txt`
- .NET report: `/tmp/zlink-dotnet-pubsub-recheck-dotnet/multi/report/perf_dotnet_multi_linux_20260813_044857_dotnet-pubsub-recheck-dotnet.txt`

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 1,642,069 msg/s | 993,911 msg/s | 60.53% |
| 256B | 1,716,723 msg/s | 1,121,145 msg/s | 65.31% |
| 1024B | 1,495,946 msg/s | 968,564 msg/s | 64.75% |
| 4096B | 554,099 msg/s | 491,393 msg/s | 88.68% |
| 65536B | 125,987 msg/s | 122,654 msg/s | 97.35% |
| 131072B | 56,912 msg/s | 58,822 msg/s | 103.36% |

산술평균은 `80.00%`다. 기존 formal 3초 측정의 `84.44%`보다 낮고 목표 `85%`를
충족하지 못한다. 이 2초 실행은 후보 진단용이므로 formal 표는 갱신하지 않았다.
두 report는 `status: complete`, result line `30/30`이다.
