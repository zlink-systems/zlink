# C++ Single PAIR / inproc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch | `core-0.13.0-bindings-performance` |
| source/report commit | `f21601ac1f` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke

`PAIR / inproc / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `perf_c_single_linux_20260825_095954_cpp-pair-inproc-core0130-smoke-c-20260825.txt`
- C++: `perf_cpp_single_linux_20260825_100003_cpp-pair-inproc-core0130-smoke-cpp-20260825.txt`

## 최초 및 최종 paired 기준선 — 3 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_100014_cpp-pair-inproc-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_100200_cpp-pair-inproc-core0130-before-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,577,838.4 | 2,556,167.4 | 99.16% | 0.192 ms | 0.095 ms | 0.495x |
| 256 | 1,751,637.0 | 1,733,568.2 | 98.97% | 1.404 ms | 1.420 ms | 1.011x |
| 1024 | 1,554,328.4 | 1,456,156.0 | 93.68% | 0.387 ms | 0.466 ms | 1.204x |
| 65536 | 371,553.4 | 71,527.6 | 19.25% | 0.010 ms | 0.030 ms | 3.000x |
| 131072 | 145,584.4 | 52,629.8 | 36.15% | 0.015 ms | 0.032 ms | 2.133x |
| 262144 | 56,268.4 | 49,432.6 | 87.85% | 0.029 ms | 0.031 ms | 1.069x |

- throughput ratio 산술평균: **72.51%** — 완화 목표 90% 미달
- mean-latency ratio 산술평균: **1.485x** — 2.0x 상한 통과
- 65536B와 131072B의 개별 latency는 2.0x를 넘지만 aggregate 판정을 뒤집지 않는다.

## 자체 진단과 Sol read-only pass

64B–1024B는 C의 93.68–99.16%지만 65536B와 131072B에서 19.25%, 36.15%로
급락하고 262144B에서 87.85%로 회복했다. C++ active path에는 64/128KiB에만 적용되는
분기가 없다. C와 C++ 모두 `zlink_msg_init_size`, payload copy, `zlink_send_part`와
`zlink_recv_part`를 사용한다. C++의 128KiB–1MiB large-message pool 조건은
`constexpr false`여서 모든 공식 크기가 native allocation 경로를 사용한다.

저장소 밖 throwaway micro-probe로 동일 프로세스에서 native
`zlink_msg_init_size+memcpy+close`와 C++ `message_t::from` 비용을 비교했다.

| Size | C++ / native storage cost |
|------|----------------------------:|
| 64 | 1.34x |
| 256 | 1.36x |
| 1024 | 1.26x |
| 65536 | 1.07x |
| 131072 | 1.04x |
| 262144 | 1.07x |

대형 storage wrapper의 1.04–1.07x 비용은 65536/131072B cliff를 설명하지 못한다.
Sol의 파일 수정 없는 독립 검토도 크기별 binding 분기가 없음을 확인했다. 정확한 원인은
native send 내부 시간, allocator cross-thread 해제, Core inproc 크기 임계와 스케줄링을
profile해야 하지만, 이는 현재 확인된 계약 안전한 binding 최적화 후보가 아니다.

다음 후보는 no-go다.

- large-message pool 활성화/A/B: 계획에서 C++ binding 적용을 명시적으로 금지
- C API 직접 호출/private perf path: 일반 public binding 경로 우회
- public builder layout/API 변경: public contract 변경
- weak liveness 또는 submit mutex 제거: escaped builder 수명과 close-vs-submit 직렬화 회귀

## POSDDD dead-pool cleanup A/B와 기각

컴파일 타임에 비활성인 large-message pool 전체를 삭제하는 후보는 dormant second
allocator, 전역 mutex와 오래된 Core 0.10.1 설명을 없애 ownership을 Core로 단일화한다는
POSDDD 가치가 있었다. 성능 후보로 계산하지 않고 코드를 임시 적용한 뒤 contract 테스트와
paired A/B를 수행했다.

- C after: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_101140_cpp-pair-inproc-core0130-dead-pool-cleanup-paired-c-20260825.txt`
- C++ after: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_101322_cpp-pair-inproc-core0130-dead-pool-cleanup-paired-cpp-20260825.txt`
- after throughput aggregate: **66.62%**
- after mean-latency aggregate: **1.706x**

active 경로는 같지만 after throughput aggregate가 기준선 72.51%보다 약 8% 상대
하락해 계획의 회귀 gate를 통과하지 못했고, 성능 이득도 분리되지 않았다. 후보는 완전히
원상복구했다. 최종 소스로 `zlink_cpp`, `cpp_perf_pair`, `test_cpp_contract_message`,
`test_cpp_contract_socket`을 다시 빌드했고 두 contract 테스트가 모두 통과했다.

기본 95%와 완화 90% 처리량 목표에 모두 미달하고 자체/Sol pass 후 채택 가능한 후보가
없으므로 **PAIR/inproc은 보류**다.
