# .NET TCP MULTI_DEALER_DEALER send wrapper pool 측정

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
| 64B | 2743.867 | 848.315 | 30.92% |
| 256B | 1515.490 | 852.895 | 56.28% |
| 1024B | 852.893 | 829.887 | 97.30% |
| 4096B | 385.281 | 318.993 | 82.79% |
| 65536B | 86.275 | 90.297 | 104.66% |
| 131072B | 54.066 | 43.795 | 81.00% |
| 산술 평균 | - | - | **75.49%** |

`Message.Allocate()`는 send 후 disposal로 invalid가 된 wrapper를 thread-local pool에서 다시 사용한다.
native frame 초기화와 ownership transfer는 기존과 같다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_151455_dotnet-dealer-dealer-tcp-send-wrapper-pool-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260812_151513_dotnet-dealer-dealer-tcp-send-wrapper-pool-dotnet.txt`
