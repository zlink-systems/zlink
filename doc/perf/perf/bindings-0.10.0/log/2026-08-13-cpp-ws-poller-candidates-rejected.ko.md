# C++ WS poller 후보 측정 결과

release Core `0.10.1`, `ws`, `MULTI_DEALER_DEALER`, duration 5초, client 100,
auto-HWM `balanced` 조건으로 측정했다. 아래 후보는 public interface를 바꾸지 않는
일반 C++ poller 경로였지만, 완료 수치가 기존 C++ WS 결과보다 낮거나 완료하지 못해
원복했다.

| 후보 | 관측 결과 | 판정 |
|---|---|---|
| socket-only cached `zlink_poll()` fast path | 64B 1,463,488 msg/s로 기존 완료 측정 1,812,936 msg/s보다 낮았고, 256B 단계가 비정상적으로 지연됨 | 원복 |
| backpressure 이후 sticky `POLLOUT` registration | 64B 1,646,359, 256B 792,762, 1,024B 569,805, 4,096B 295,959, 65,536B 39,475 msg/s로 기존 완료 측정보다 모두 낮음 | 원복 |

원복 뒤 `ctest --test-dir bindings/cpp/build --output-on-failure`의
`perf_cpp_multi_dealer_dealer_runtime_smoke`가 통과했다.
