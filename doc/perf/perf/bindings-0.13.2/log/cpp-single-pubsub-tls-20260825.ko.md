# C++ Single PUBSUB / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `0babb638e98828e4cf801bfbe619e4c0ad5c51d1`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `1379.40 Kmsg/s`; C++ `1266.57 Kmsg/s` (91.82%).
- clean C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_232737_cpp-pubsub-tls-local0132-final-clean-paired5-c-20260825.txt`
- clean C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_233109_cpp-pubsub-tls-local0132-final-clean-paired5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 88.09% | 1.03x |
| 256B | 95.53% | 1.07x |
| 1024B | 94.03% | 1.08x |
| 65536B | 88.99% | 1.11x |
| 131072B | 88.96% | 1.10x |
| 262144B | 87.11% | 1.15x |

## 개선 pass와 최종 판정

- C1 — `read_subscription_message()`가 temporary native message/adopt handoff 대신 `message_t` 저장소로 직접 수신하도록 구현했다. message/socket/behavior/flow contract와 6-size paired run은 통과했지만 aggregate **90.86%**로 baseline 92.13%보다 회귀하여 원복했다.
- C2 — `topic_message_t`의 single part를 retained vector에 바로 materialize하는 내부 경로를 C1 위에서 구현했다. 계약 테스트는 통과했으나 full pair **92.10%**는 baseline보다 유의미한 개선이 아니어서 원복했다.
- C3 — 전역 large-message pool bypass를 C1/C2 위에서 진단했다. TLS 128/256KiB는 94.26%/99.83%, full TLS pair는 93.07%까지 보였지만, transport-agnostic policy의 inproc 128KiB가 `112481.8 -> 90718.2 Kmsg/s`(**80.65%, -19.35%p**)로 회귀했다. 256KiB 102.26%가 이를 상쇄하지 못하므로 원복했다.
- Sol 최종 review: C1/C2/C3 모두 폐기하고, direct publish 재구현, topic snapshot/validation 생략, `no_drop`/HWM/subscription/readiness 변경, outbound mutex/liveness 제거, transport-specific·thread-local·lock-free pool, cap/64KiB 정책 변경은 contract/resource/harness no-go다. 추가 안전 후보는 없다.
- 원복 source의 authoritative secure C/C++ 5-run pair는 throughput aggregate **90.45%**, latency median **1.09x**다. latency는 통과하지만 simple one-way strict 95% throughput 목표에는 미달하므로 Sol 승인에 따라 상태를 **보류(미달 90.45%)**로 확정하고 다음 pattern으로 진행한다.
