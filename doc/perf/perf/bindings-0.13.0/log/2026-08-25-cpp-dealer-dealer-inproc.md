# C++ Single DEALER_DEALER / inproc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `7d77747de6` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 paired 기준선 — 3 runs

`DEALER_DEALER / inproc / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_120530_cpp-dealer-dealer-inproc-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_120530_cpp-dealer-dealer-inproc-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_120542_cpp-dealer-dealer-inproc-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_120542_cpp-dealer-dealer-inproc-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,880,099.6 | 1,552,154.6 | 82.56% | 0.098 ms | 0.047 ms | 0.480x |
| 256 | 1,375,036.0 | 1,321,220.4 | 96.09% | 1.156 ms | 1.228 ms | 1.062x |
| 1024 | 1,410,280.8 | 1,102,638.6 | 78.19% | 0.228 ms | 0.213 ms | 0.934x |
| 65536 | 358,801.2 | 62,576.0 | 17.44% | 0.007 ms | 0.032 ms | 4.571x |
| 131072 | 146,303.8 | 53,142.2 | 36.32% | 0.014 ms | 0.032 ms | 2.286x |
| 262144 | 49,412.2 | 28,703.0 | 58.09% | 0.034 ms | 0.053 ms | 1.559x |

- throughput ratio 산술평균: **61.45%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.815x** — C++ 2.0x 상한 통과; 64KiB cell은 4.571x outlier

## 공개 경로 검토

PAIR/inproc 및 PUBSUB/inproc과 같은 64/128KiB 임계 구간의 급락이 DEALER terminal에서도 재현됐다.
DEALER 고유 경로는 public builder와 Core send handoff뿐이며 pool state, single-part submit과 payload
ownership 재사용은 이미 적용돼 있다. raw C/private path, public ownership/close 변경, weak/mutex 제거,
큰 메시지 pool 추가·활성화는 no-go다. 현재 근거는 binding 바깥의 native message handoff, Core inproc
queue 및 cross-thread allocator/scheduler 상호작용을 가리키며 Core profile 없이 단정하지 않는다.

처리량이 완화 목표보다 충분히 낮고 안전한 binding 후보가 없으므로 source 변경 없이
**DEALER_DEALER/inproc은 보류**다. 최종 5회는 목표 경계가 아니므로 요구되지 않는다.
