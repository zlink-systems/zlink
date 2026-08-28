# C++ Multi Router↔Router 0.10.1 vs 0.13.2 (2026-08-26)

## 비교 범위

- C++ Multi `ROUTER_ROUTER_SENDSEND`, `ROUTER_ROUTER_REQREP`
- TCP, 100 clients, server/client I/O threads 4/4
- 64, 256, 1024, 4096, 65536, 131072 bytes
- 2초×3회, size별 median
- 0.10.1: release Core 0.10.1 + 0.10.1 성능 작업 종료 시점의 clean C++ worktree
- 0.13.2: local Core 0.13.2 + 현재 C++ worktree

이 비교는 각 버전의 Core, C++ binding, perf harness와 auto-HWM 정책을 포함한 end-to-end
버전 비교다. Core만 단독으로 교체한 ABI-isolated 비교는 아니다.

## Throughput

단위는 Kops/s이며 비율은 `0.13.2 / 0.10.1`이다.

| Pattern | Size | 0.10.1 | 0.13.2 | 비율 |
|---|---:|---:|---:|---:|
| SENDSEND | 64 | 174.707 | 167.261 | 95.74% |
| SENDSEND | 256 | 187.013 | 160.307 | 85.72% |
| SENDSEND | 1024 | 174.715 | 152.066 | 87.04% |
| SENDSEND | 4096 | 163.819 | 135.353 | 82.62% |
| SENDSEND | 65536 | 30.618 | 29.568 | 96.57% |
| SENDSEND | 131072 | 18.889 | 19.875 | 105.22% |
| REQREP | 64 | 87.432 | 88.686 | 101.43% |
| REQREP | 256 | 81.879 | 80.989 | 98.91% |
| REQREP | 1024 | 80.070 | 72.837 | 90.97% |
| REQREP | 4096 | 73.549 | 65.107 | 88.52% |
| REQREP | 65536 | 19.970 | 25.095 | 125.66% |
| REQREP | 131072 | 13.649 | 16.718 | 122.49% |

- SENDSEND 6-size 산술평균 비율: **92.15%** (0.13.2가 7.85% 낮음)
- REQREP 6-size 산술평균 비율: **104.67%** (0.13.2가 4.67% 높음)

## Mean latency

비율은 `0.13.2 / 0.10.1`이며 낮을수록 좋다.

| Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 중앙 비율 |
|---|---:|---:|---:|---:|---:|---:|---:|
| SENDSEND | 1.03x | 1.16x | 1.14x | 1.20x | 1.08x | 1.25x | **1.15x** |
| REQREP | 1.30x | 1.36x | 1.47x | 1.56x | 1.51x | 1.51x | **1.49x** |

## 판정

- SENDSEND는 256B–4096B에서 82.62%–87.04%로 명확한 처리량 회귀가 있다.
- REQREP 처리량은 small/mid 감소를 64KiB·128KiB 증가가 상쇄하지만, mean latency가 모든
  size에서 악화되어 latency 회귀가 남아 있다.
- 0.10.1은 message-size 기반 auto-HWM 값, 0.13.2는 byte-HWM 기반 client/server budget을
  사용하므로 HWM 정책 변화도 이 end-to-end 차이에 포함된다.

## 보고서

- 0.10.1: `/home/hep7hep7/project/zlink-perf-0.10.1-final/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260826_133428_compare-v0101-router-router-tcp-r3-20260826.txt`
- 0.13.2: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260826_133752_compare-v0132-router-router-tcp-r3-20260826.txt`
- 두 보고서 모두 success 12, fail 0, RESULT 60/60, `status: complete`다.
