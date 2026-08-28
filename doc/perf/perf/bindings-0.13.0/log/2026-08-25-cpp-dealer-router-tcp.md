# C++ Single DEALER_ROUTER / tcp — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `09f85f1848` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 paired 기준선 — 3 runs

`DEALER_ROUTER / tcp / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_121236_cpp-dealer-router-tcp-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_121236_cpp-dealer-router-tcp-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_121253_cpp-dealer-router-tcp-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_121253_cpp-dealer-router-tcp-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 18/18 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

3회 기준선 throughput ratio 산술평균은 84.80%, mean-latency ratio 산술평균은 1.147x였다.
routed one-way 목표 85% 경계(약 5 percentage points 이내)이므로 C→C++ 순서의 최종 5회
중앙값을 실행했다.

## 최종 paired 판정 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_121448_cpp-dealer-router-tcp-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_121729_cpp-dealer-router-tcp-core0130-final5-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,444,512.0 | 2,039,914.0 | 83.45% | 0.318 ms | 0.262 ms | 0.824x |
| 256 | 1,325,611.4 | 1,359,406.0 | 102.55% | 1.275 ms | 1.263 ms | 0.991x |
| 1024 | 524,598.8 | 531,096.8 | 101.24% | 0.921 ms | 0.916 ms | 0.995x |
| 65536 | 28,915.4 | 22,612.6 | 78.20% | 0.336 ms | 0.456 ms | 1.357x |
| 131072 | 20,182.0 | 16,657.8 | 82.54% | 0.303 ms | 0.373 ms | 1.231x |
| 262144 | 12,485.8 | 11,451.2 | 91.71% | 0.328 ms | 0.355 ms | 1.082x |

- throughput ratio 산술평균: **89.95%** — routed one-way 목표 85% 통과
- mean-latency ratio 산술평균: **1.080x** — C++ 2.0x 상한 통과
- 64KiB는 78.20%로 개별 최소 80%보다 1.80 percentage points 낮다. 이 계획의 transport
  throughput gate는 six-size aggregate mean이므로, 해당 값을 병목 기록으로 남기고 종합 판정을
  미달로 바꾸지 않는다.

## 공개 경로 검토

활성 수신은 Router가 public `recv(routing_id_t&, message_t&, dontwait)`로 source routing id와
payload를 함께 반환하는 경로다. routing id의 공개 결과물과 `message_t` payload ownership,
poller의 readiness 소비, transient failure 처리는 terminal 계약이므로 생략하거나 private/C API
경로로 바꿀 수 없다. sender의 public `send_routed`, wire stop token, socket close/liveness와
outbound serialization도 C 기준과 같은 활성 구간 의미를 보존한다.

raw native message/routing-id 재사용, public ownership 변경, weak/mutex 제거, 큰 메시지 전용
pool 증설은 계약 또는 측정 정책을 위반한다. 5회 최종 결과가 목표와 latency 상한을 모두
충족했으므로 source 변경 없이 **DEALER_ROUTER/tcp는 통과**로 확정한다.
