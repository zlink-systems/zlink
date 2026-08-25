# C++ Single PUBSUB / tls — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `6fe83ce277` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`PUBSUB / tls / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_105940_cpp-pubsub-tls-core0130-smoke-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_105940_cpp-pubsub-tls-core0130-smoke-cpp-20260825.txt`

최초 paired 3회 결과는 throughput 산술평균 **90.71%**, mean-latency 산술평균 **1.165x**였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_105949_cpp-pubsub-tls-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_110150_cpp-pubsub-tls-core0130-before-cpp-20260825.txt`

3회 처리량은 완화 목표보다 0.71%p 높았지만 secure transport이자 목표 경계이므로 source 변경 전에
동일 manifest C→C++ final 5회를 사전 결정해 실행했다.

## 자체 및 Sol read-only 검토

TLS 전용 binding 작업은 `tls://` endpoint와 측정 전 인증서 설정뿐이다. 활성 PUBSUB loop는
transport 분기 없이 공통 publish/subscribe를 사용하며 `zlink_publish_part`와
`zlink_subscribe_part` 아래의 TLS record 및 암복호화는 Core 0.13.0 소유다.

공통 경로의 topic NUL 검증과 결과 복사, weak liveness, close-submit gate, 실패 시 message
ownership 복구는 public 계약에 필요하다. C/private path, `subscribe_part` harness 우회,
public API/layout 변경, mutex/weak 제거, pool 확대와 large-message pool은 no-go다.
자체와 Sol 검토 모두 채택 가능한 source 후보가 없다고 결론냈다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_110413_cpp-pubsub-tls-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_110733_cpp-pubsub-tls-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,438,001.6 | 1,255,218.0 | 87.29% | 0.333 ms | 0.230 ms | 0.691x |
| 256 | 761,053.6 | 722,897.2 | 94.99% | 1.896 ms | 1.968 ms | 1.038x |
| 1024 | 284,118.0 | 259,237.4 | 91.24% | 1.636 ms | 1.820 ms | 1.112x |
| 65536 | 11,448.6 | 10,183.0 | 88.95% | 0.869 ms | 0.978 ms | 1.125x |
| 131072 | 6,625.8 | 5,833.6 | 88.04% | 0.889 ms | 1.004 ms | 1.129x |
| 262144 | 3,511.2 | 3,005.4 | 85.59% | 1.128 ms | 1.315 ms | 1.166x |

- throughput ratio 산술평균: **89.35%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.044x** — C++ 2.0x 상한 통과

사전 결정한 secure final 5회 세트가 throughput gate에 미달하고, 자체/Sol pass 후에도 안전한
binding 후보가 없으므로 source 변경 없이 **PUBSUB/tls는 보류**다.
