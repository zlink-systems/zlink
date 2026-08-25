# C++ Single PUBSUB / tcp — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch | `core-0.13.0-bindings-performance` |
| source/report commit | `9c8a9fc645` |
| source version | `0.13.2` (최신 `main` 병합 결과) |
| Core runtime version | `0.13.0` |
| Core release tag | `core/v0.13.0` |
| Core revision | `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| Core runtime | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## 최초 paired 기준선

조건: `PUBSUB / tcp / 64,256,1024,65536,131072,262144B`, duration 5초,
runs 3 중앙값, io_threads 1, auto-HWM balanced.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_085644_cpp-pubsub-tcp-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_085846_cpp-pubsub-tcp-core0130-before-cpp-20260825.txt`
- status: 둘 다 `complete`
- size별 throughput ratio: 79.51%, 84.58%, 91.06%, 89.92%, 96.13%, 96.47%
- throughput aggregate: **89.61%**
- mean-latency aggregate: **1.141x**

## 자체 hot-path pass

현재 코드는 이전 C++ PUBSUB pass에서 확인한 다음 항목을 이미 포함한다.

- C에 없는 context auto-HWM recalculation 제거
- PUB/XPUB publish state 조립과 subscription 오류 변환 중복 제거
- topic validation이 callback-state lazy allocation보다 먼저 일어나는 error-order 보존
- C++ library와 최종 perf consumer에 대한 LTO 배선, 기본 `OFF`

이전 LTO A/B는 aggregate throughput `89.50%`(OFF) 대 `89.16%`(ON), latency
`1.060x` 대 `1.079x`로 ON이 악화되어 no-go였다. 현재 build도 `ENABLE_LTO=OFF`다.

현재 publish hot path는 public `publish(topic).message(msg).submit()` fluent builder,
pooled operation state, topic validation/owned copy, weak socket lifetime 검사, close/multipart
interleave gate를 지난다. topic `"bench"`는 SSO라 heap allocation은 없으며, validation과
owned copy를 제거하면 invalid-topic error priority 또는 builder escape lifetime 계약을
깨뜨린다. submit weak check와 mutex도 socket보다 오래 사는 builder와 close 동시성 계약상
제거할 수 없다. C API 또는 `subscribe_part()` 직접 호출은 측정 public surface를 바꾸므로
후보에서 제외했다.

## Sol read-only pass

Sol은 파일을 수정하지 않고 현재 0.13.0 report와 코드를 검토했다.

- 안전성과 효과를 함께 확신할 library 변경은 없음
- topic pointer/cache/token 도입, weak liveness 제거, mutex 제거, publish 전용 state/API는
  contract 또는 POSDDD를 해치므로 no-go
- `topic_message_t` 재사용과 lazy single-part 저장은 이미 적용되어 있고, native handoff를
  더 줄이는 변경은 ownership 실패 경로 profile 근거가 없어 보류
- 유일한 좁은 후보는 C++ harness의 반복 `parts()` 호출을 public
  `is_single_part()/first_part()`로 바꿔 vector materialization을 피하는 parity A/B

## Single-part harness 후보 A/B와 기각

후보는 `subscribe(topic_message_t&, flags)` surface, topic 검사, payload ownership, stop token
의미를 유지하고 단일 part 접근만 바꿨다. `test_cpp_contract_message`와 perf target build는
통과했다.

- 공식 3-run 진단 report:
  - C: `perf_c_single_linux_20260825_090937_cpp-pubsub-tcp-core0130-singlepart-ab-c-20260825.txt`
  - C++: `perf_cpp_single_linux_20260825_091041_cpp-pubsub-tcp-core0130-singlepart-ab-cpp-20260825.txt`
- 동일한 두 바이너리를 교차 실행한 64B 결과는 후보가 +0.75~+2.60%였으나,
  256B는 두 교차쌍 모두 -6.45%, -16.17%였고 1024B는 -0.26%, +3.89%로 방향이 섞였다.
- aggregate 개선이 유의미하고 반복 가능하다는 채택 조건을 만족하지 못해 후보를 전부
  되돌리고 공식 baseline binary를 다시 build했다.

## 최종 경계 판정 — 5 runs

계획서의 최종·경계 조건에 따라 변경 없는 `ENABLE_LTO=OFF` build로 C 다음 C++을
5회 측정했다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_090156_cpp-pubsub-tcp-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_090516_cpp-pubsub-tcp-core0130-final5-cpp-20260825.txt`
- status: 둘 다 `complete`, result 30/30
- Core runtime/revision/release tag와 Effective Options 일치
- publisher 1 / subscriber 1, memory guard 해당 없음

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,344,760.0 | 1,148,792.4 | 85.43% | 0.116 ms | 0.147 ms | 1.267x |
| 256 | 1,034,053.6 | 1,002,981.0 | 97.00% | 1.485 ms | 1.500 ms | 1.010x |
| 1024 | 581,207.8 | 544,902.0 | 93.75% | 0.944 ms | 0.844 ms | 0.894x |
| 65536 | 33,411.0 | 30,274.8 | 90.61% | 0.283 ms | 0.342 ms | 1.208x |
| 131072 | 21,363.6 | 17,454.8 | 81.70% | 0.273 ms | 0.340 ms | 1.245x |
| 262144 | 11,904.6 | 13,253.0 | 111.33% | 0.330 ms | 0.300 ms | 0.909x |

- throughput ratio 산술평균: **93.30%**
- mean-latency ratio 산술평균: **1.089x** — 2.0x 상한 통과
- 131072B는 개별 최소 85% 미달이지만 aggregate gate를 낮추지 않고 outlier로 기록

기본 목표 95%에는 미달한다. 자체 pass와 Sol pass에서 남은 비용이 public fluent builder와
topic/lifetime/close 계약에 속하고, 안전한 후보의 A/B도 기각됐으므로 계획서가 허용한 C++
단순 one-way 완화 목표 **90%를 이 셀에 선택**한다. aggregate 93.30%가 완화 목표를
충족하므로 최종 판정은 **통과**다.
