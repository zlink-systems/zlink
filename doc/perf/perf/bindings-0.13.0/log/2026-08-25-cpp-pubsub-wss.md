# C++ Single PUBSUB / wss — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `914cba8307` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`PUBSUB / wss / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_104707_cpp-pubsub-wss-core0130-smoke-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_104707_cpp-pubsub-wss-core0130-smoke-cpp-20260825.txt`

최초 paired 3회 결과는 throughput 산술평균 **89.34%**, mean-latency 산술평균 **1.043x**였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_104716_cpp-pubsub-wss-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_104919_cpp-pubsub-wss-core0130-before-cpp-20260825.txt`

WSS는 secure transport이고 3회 처리량이 완화 목표 90%의 경계에 있어, source 변경 전에
동일 manifest C→C++ final 5회를 사전 결정해 실행했다.

## 자체 및 Sol read-only 검토

WSS 추가 binding 작업은 인증서/CA 설정과 연결 전 setup 한 번뿐이다. 활성 PUBSUB loop는
transport 분기 없이 공통 publish/subscribe를 사용하며, `zlink_publish_part`와
`zlink_subscribe_part` 아래의 WebSocket framing, TLS record와 암복호화는 Core 0.13.0 소유다.

공통 binding 경로는 topic NUL 검증·SSO topic copy, pooled builder와 single-part borrow,
성공 시에만 receive 결과를 commit하는 ownership을 이미 적용한다. 검증/복사를 생략하거나
mutex/weak을 제거하면 public error·topic·close-vs-submit 계약이 깨진다. C/private path,
`subscribe_part` harness 전환, pool 확대와 large-message pool도 측정 경로 또는 안전 계약을
바꾸므로 no-go다. 자체와 Sol 검토 모두 채택 가능한 source 후보가 없다고 결론냈다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_105139_cpp-pubsub-wss-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_105457_cpp-pubsub-wss-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,298,186.8 | 1,122,658.0 | 86.48% | 59.121 ms | 66.909 ms | 1.132x |
| 256 | 628,891.8 | 561,771.6 | 89.33% | 41.920 ms | 50.861 ms | 1.213x |
| 1024 | 210,526.2 | 188,114.6 | 89.35% | 36.116 ms | 39.533 ms | 1.095x |
| 65536 | 8,676.2 | 7,728.6 | 89.08% | 15.412 ms | 21.752 ms | 1.411x |
| 131072 | 5,129.4 | 5,120.8 | 99.83% | 11.427 ms | 11.863 ms | 1.038x |
| 262144 | 2,921.0 | 2,784.6 | 95.33% | 10.981 ms | 11.477 ms | 1.045x |

- throughput ratio 산술평균: **91.57%** — C++ 단순 one-way 완화 목표 90% 통과
- mean-latency ratio 산술평균: **1.156x** — C++ 2.0x 상한 통과

소스 변경 없이 secure final 5회 paired 세트가 두 gate를 통과했으므로
**PUBSUB/wss는 완화 목표 90%로 통과**다.
