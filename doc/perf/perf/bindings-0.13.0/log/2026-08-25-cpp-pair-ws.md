# C++ Single PAIR / ws — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch | `core-0.13.0-bindings-performance` |
| source/report commit | `06f6821550` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke

`PAIR / ws / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `perf_c_single_linux_20260825_091840_cpp-pair-ws-core0130-smoke-c-20260825.txt`
- C++: `perf_cpp_single_linux_20260825_091842_cpp-pair-ws-core0130-smoke-cpp-20260825.txt`

## 최초 paired 기준선 — 3 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_091849_cpp-pair-ws-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_092032_cpp-pair-ws-core0130-before-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- size별 throughput ratio: 77.24%, 96.08%, 95.55%, 97.04%, 93.43%, 96.37%
- throughput aggregate: **92.62%**
- mean-latency aggregate: **1.112x**

## 자체 pass와 Sol read-only pass

PAIR/ws의 active send/recv loop는 tcp와 같은 `pair_socket_t` public 경로를 사용한다.
`ws` transport 문자열은 측정 전 setup에만 쓰이고, 실제 송수신은 공통
`zlink_send_part`/`zlink_recv_part` 경계로 들어간다. WS framing은 고정된 Core 0.13.0
runtime이 소유하므로 binding에 WS 전용 hot path 분기가 없다.

PAIR/tcp pass에서 다음 항목을 이미 검증했다.

- C++ PAIR의 불필요한 `.flags(none)` setter와 C와 다른 receive counter memory order 수정
- C7 fixed-capacity TLS pool crossover 기각
- submit mutex 제거, weak lifetime 검사 제거, large-message pool, C API 직접 호출 no-go

Sol은 현재 ws report와 코드를 파일 수정 없이 다시 검토했고 새 WS-specific binding 후보가
없다고 확인했다. 64B의 71~77%는 공개 fluent builder와 message ownership/lifetime/close
계약의 고정 비용이 WS/Core 처리량에 비해 두드러지는 outlier다. HWM 변경, WS 우회 경로,
반복 수 조작으로 통과시키지 않는다.

## 최종 경계 판정 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_092245_cpp-pair-ws-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_092532_cpp-pair-ws-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,696,871.6 | 1,932,848.4 | 71.67% | 40.558 ms | 49.239 ms | 1.214x |
| 256 | 1,111,650.0 | 1,009,893.8 | 90.85% | 30.372 ms | 42.177 ms | 1.389x |
| 1024 | 371,333.2 | 368,880.0 | 99.34% | 21.659 ms | 25.085 ms | 1.158x |
| 65536 | 18,162.0 | 17,983.8 | 99.02% | 6.749 ms | 7.768 ms | 1.151x |
| 131072 | 12,852.8 | 11,628.8 | 90.48% | 0.504 ms | 0.525 ms | 1.042x |
| 262144 | 7,629.0 | 7,572.4 | 99.26% | 0.541 ms | 0.536 ms | 0.991x |

- throughput ratio 산술평균: **91.77%**
- mean-latency ratio 산술평균: **1.157x** — 2.0x 상한 통과
- 64B는 개별 최소 85% 미달 outlier지만 계획의 aggregate 판정을 뒤집지 않는다.

기본 95% 목표에는 미달한다. 동일 PAIR 공통 경로의 자체/Sol pass를 완료했고 새 WS 후보가
없으므로 계획서가 허용한 C++ 단순 one-way 완화 목표 **90%를 선택**한다. 최종 aggregate
91.77%가 이를 충족해 **PAIR/ws는 통과**다.
