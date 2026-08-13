# .NET TCP MULTI_DEALER_ROUTER send wrapper pool 측정

## 조건

- Core: release `0.10.1`
- 순서: C 단독 실행 뒤 .NET 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kops/s | .NET Kops/s | .NET/C |
|---|---:|---:|---:|
| 64B | 192.827 | 137.300 | 71.20% |
| 256B | 179.503 | 140.783 | 78.43% |
| 1024B | 143.084 | 143.740 | 100.46% |
| 4096B | 169.740 | 136.884 | 80.64% |
| 65536B | 38.182 | 29.768 | 77.96% |
| 131072B | 21.310 | 24.067 | 112.94% |
| 산술 평균 | - | - | **86.94%** |

`Message.Allocate()`의 thread-local send wrapper 재사용을 적용했다. public interface와 message
ownership transfer는 변경하지 않았다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_151617_dotnet-dealer-router-tcp-send-wrapper-pool-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260812_151633_dotnet-dealer-router-tcp-send-wrapper-pool-dotnet.txt`
