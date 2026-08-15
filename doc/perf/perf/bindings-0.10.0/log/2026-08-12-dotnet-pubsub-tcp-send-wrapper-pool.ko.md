# .NET TCP MULTI_PUBSUB send wrapper pool 측정

## 조건

- Core: release `0.10.1`
- 순서: C 단독 실행 뒤 .NET 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | .NET Kmsg/s | .NET/C |
|---|---:|---:|---:|
| 64B | 1584.461 | 886.294 | 55.94% |
| 256B | 1513.722 | 966.295 | 63.84% |
| 1024B | 1226.619 | 888.714 | 72.45% |
| 4096B | 524.219 | 413.475 | 78.88% |
| 65536B | 110.469 | 109.886 | 99.47% |
| 131072B | 54.400 | 61.401 | 112.87% |
| 산술 평균 | - | - | **80.58%** |

`Message.Allocate()`의 thread-local send wrapper 재사용을 적용했다. public interface와 message
ownership transfer는 변경하지 않았다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_151811_dotnet-pubsub-tcp-send-wrapper-pool-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260812_151824_dotnet-pubsub-tcp-send-wrapper-pool-dotnet.txt`
