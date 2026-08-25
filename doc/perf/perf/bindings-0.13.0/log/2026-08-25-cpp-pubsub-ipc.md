# C++ Single PUBSUB / ipc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `d081d00846` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`PUBSUB / ipc / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_111935_cpp-pubsub-ipc-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_111935_cpp-pubsub-ipc-core0130-smoke-cpp-20260825.txt`
- 최초 C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_111948_cpp-pubsub-ipc-core0130-before-c-20260825.txt`
- 최초 C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_112148_cpp-pubsub-ipc-core0130-before-cpp-20260825.txt`

최초 3회 paired 결과는 throughput 산술평균 **89.47%**, mean-latency 산술평균 **1.179x**였다.
90% 경계이므로 source 변경 전에 동일 manifest C→C++ final 5회를 사전 결정해 실행했다.

## 자체 및 Sol read-only 검토

IPC 전용 코드는 endpoint 생성, bind/`last_endpoint` 해석과 연결 준비까지이며 활성 PUBSUB
구간은 transport 공통이다. 송신은 topic 검증·pooled state 뒤 `zlink_publish_part`, 수신은
`zlink_subscribe_part` 뒤 success-only `topic_message_t` commit으로 내려간다. IPC transport와
queue는 Core 0.13.0 소유다.

`bench` topic은 SSO 범위이고 operation-state/string capacity 및 single-part storage도 이미
재사용한다. topic cache/검증 생략, C/private path 또는 `subscribe_part` harness 우회, public
ownership 변경, mutex/weak 제거, pool 확대·large-message pool은 모두 계약 또는 측정 경로를
바꾸므로 no-go다. 자체와 Sol 검토 모두 채택 후보가 없다고 결론냈다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_112404_cpp-pubsub-ipc-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_112715_cpp-pubsub-ipc-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,350,251.4 | 1,219,104.8 | 90.29% | 0.076 ms | 0.073 ms | 0.961x |
| 256 | 1,127,072.0 | 1,073,302.6 | 95.23% | 1.344 ms | 1.378 ms | 1.025x |
| 1024 | 685,774.4 | 704,387.0 | 102.71% | 0.673 ms | 0.664 ms | 0.987x |
| 65536 | 37,903.6 | 34,459.2 | 90.91% | 0.243 ms | 0.269 ms | 1.107x |
| 131072 | 24,250.6 | 23,781.2 | 98.06% | 0.243 ms | 0.252 ms | 1.037x |
| 262144 | 15,760.8 | 15,188.0 | 96.37% | 0.248 ms | 0.257 ms | 1.036x |

- throughput ratio 산술평균: **95.60%** — C++ 단순 one-way 기본 목표 95% 통과
- mean-latency ratio 산술평균: **1.025x** — C++ 2.0x 상한 통과

소스 변경 없이 경계 규칙의 final 5회 paired 세트가 기본 두 gate를 통과했으므로
**PUBSUB/ipc는 기본 목표 95%로 통과**다.
