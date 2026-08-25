# C++ Single PUBSUB / inproc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `ba17129168` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 paired 기준선 — 3 runs

`PUBSUB / inproc / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_111229_cpp-pubsub-inproc-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_111229_cpp-pubsub-inproc-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_111239_cpp-pubsub-inproc-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_111439_cpp-pubsub-inproc-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,178,065.8 | 1,088,549.8 | 92.40% | 0.023 ms | 0.022 ms | 0.957x |
| 256 | 930,032.0 | 908,050.8 | 97.64% | 0.036 ms | 0.036 ms | 1.000x |
| 1024 | 902,978.8 | 783,084.0 | 86.72% | 0.039 ms | 0.030 ms | 0.769x |
| 65536 | 333,137.6 | 75,478.2 | 22.66% | 0.009 ms | 0.030 ms | 3.333x |
| 131072 | 138,593.8 | 63,768.0 | 46.01% | 0.016 ms | 0.029 ms | 1.812x |
| 262144 | 63,330.0 | 48,243.2 | 76.18% | 0.026 ms | 0.032 ms | 1.231x |

- throughput ratio 산술평균: **70.27%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.517x** — 2.0x 상한 통과
- 65536B latency 3.333x는 aggregate 판정을 뒤집지 않는 개별 outlier다.

## 자체 및 Sol read-only 검토

PUBSUB 고유의 topic 검증과 `bench` SSO 문자열 복사, success-only `topic_message_t` commit은
크기 독립이다. 단일 part receive는 lazy move와 retained capacity로 payload vector copy를
피하며, sender도 `message_t::from`→pooled builder→native single-part submit 공통 경로로
inproc binding 분기가 없다.

PAIR/inproc과 PUBSUB/inproc 모두 64/128KiB에서 급락하고 256KiB에서 회복한다. 저장소 밖
native allocation+copy micro-probe의 C++/native 비율은 64–256KiB에서 1.04–1.07x여서
topic 처리, subscribe ownership, allocation wrapper 단독 원인이 아니다. 현재 근거가 가리키는
범위는 binding 밖의 common native-message handoff, Core inproc queue, cross-thread allocator와
scheduler의 size-threshold 상호작용이다. Core 내부 profile 없이 하나로 단정하지 않는다.

다음 후보는 no-go다.

- `subscribe_part`, C API 또는 private perf path로 측정 경로 우회
- topic validation/result 또는 public `topic_message_t` ownership 제거
- mutex/weak 제거
- disabled large-message pool 활성화 또는 새 pool 도입

자체/Sol pass 후에도 public 계약을 지키는 binding 후보가 없으며 처리량이 목표보다 크게
낮으므로 source 변경 없이 **PUBSUB/inproc은 보류**다. 정확한 원인 확인은 Core/native call
구간 profile을 별도 진단 과제로 분리한다.
