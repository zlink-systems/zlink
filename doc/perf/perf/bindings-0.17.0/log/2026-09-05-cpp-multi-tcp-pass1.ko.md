# C++ Multi `tcp` 자체 hot-path 개선 pass 1 — after 측정 — Core 0.17.0

2026-09-05 03:58~04:14 KST에 §7.4 9~10단계의 자체 hot-path 개선 pass 1을 수행하고 after를
한 번 측정했다. 대상은 before 측정(`log/2026-09-05-cpp-multi-tcp-before.ko.md`)에서 aggregate가
목표 미달이었던 `tcp` 3 pattern(`MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`,
`MULTI_ROUTER_ROUTER_REQREP`)이다. `MULTI_PUBSUB`은 이 pass에서 다루지 않았고 before 값 그대로다.
§7.4 11단계의 Sol read-only 리뷰와 두 번째 개선 pass는 아직 수행하지 않았다.

## Manifest

| 항목 | 값 |
|------|----|
| source | `main` / `053a568ddd` + pass 1 작업 tree 변경(`bindings/cpp/**` 6개 파일, 아래 "변경") |
| 작업 worktree | `/home/hep7hep7/project/zlink-wt-cpp-perf` (job `c016`) |
| Core runtime | `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` (before와 동일 artifact) |
| Core build / version / revision | local `Release` + LTO / `0.17.0` / `053a568dddd04a8cb8efbc2f9ee9c0df915f4e47` (`core_dirty=0`) |
| binding | `zlink_cpp` 0.17.0, GCC/G++ 13.3.0 |
| host | before와 같은 WSL2 host, 같은 boot session (`log/2026-09-05-environment.ko.md`) |
| CPU 상태 | `--pin-cpu` 없음, 동시 perf process 없음 |
| pair tag | after report는 `--results-tag` 없이 실행했다. paired C는 before의 `p1cpp` C report를 그대로 사용한다(§7.3: 같은 Core runtime·host session, binding 변경만 다름) |

## Paired C 규칙 적용

- after 측정은 before의 C report(`perf_c_multi_linux_20260905_{034919,035123,035218}_p1cpp.txt`,
  03:49~03:52 KST)를 기준으로 비교했다. after 실행 시각은 04:13 KST로 C 측정과 21~24분 차이다.
- host boot, Core runtime, revision, 성능 환경이 바뀌지 않았고(`META,core_runtime`,
  `META,core_revision`, `META,core_dirty` 동일), 시작 시점 load average는 0.77 0.55 1.08로
  before 세션(1분 0.95~6.00)보다 낮았다. §7.3의 C 재측정 조건에는 해당하지 않아 C를 다시 측정하지 않았다.
- after는 3 pattern을 한 report에 순차 실행했다(pattern cooldown 3000 ms). before는 pattern마다
  별도 report였다. Effective Options는 before C++ report와 모두 같다(`clients: 100`,
  `default_clients: 100`, I/O 4/4, auto-HWM balanced, `duration_seconds: 5`, `runs: 1`).

## 명령

```bash
# after (작업 worktree에서 실행)
ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 \
  bash bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern MULTI_DEALER_DEALER,MULTI_DEALER_ROUTER_REQREP,MULTI_ROUTER_ROUTER_REQREP \
  --transports tcp --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1
```

## 비용 위치 확인 (callgrind)

callgrind는 10 clients, 1024B, 1초의 좁은 위치 확인용 측정이다. 처리량 판정에는 쓰지 않는다.
report(`/home/hep7hep7/project/zlink-wt-cpp-perf/bindings/cpp/perf/results/multi/report/`):

| 용도 | report |
|------|--------|
| profile baseline (DD+DR, 1024B, 10 clients, 1초) | `perf_cpp_multi_linux_20260905_035817_cpp_pass1_profile_baseline.txt` |
| callgrind before REQREP / DD | `perf_cpp_multi_linux_20260905_035950_cpp_pass1_callgrind_rr.txt`, `perf_cpp_multi_linux_20260905_040014_cpp_pass1_callgrind_dd.txt` |
| 후보 probe (bundle, deferred map) | `perf_cpp_multi_linux_20260905_040639_cpp_pass1_bundle_probe.txt`, `perf_cpp_multi_linux_20260905_041005_cpp_pass1_deferred_map_probe.txt` |
| callgrind after REQREP / DD | `perf_cpp_multi_linux_20260905_041131_cpp_pass1_callgrind_rr_after.txt`, `perf_cpp_multi_linux_20260905_041158_cpp_pass1_callgrind_dd_after.txt` |

작업 summary는 REQREP callgrind 행을 `ROUTER_ROUTER_REQREP`로 표기했지만, 위 `callgrind_rr` /
`callgrind_rr_after` report의 `patterns`는 `MULTI_DEALER_ROUTER_REQREP`다. 어느 REQREP pattern의
값인지는 두 번째(리뷰) pass에서 확인한다. 두 REQREP pattern의 binding 경로는 같은 completion
대기 구조를 쓰므로 아래 비용 위치 판단 자체는 달라지지 않는다.

### 경로별 총량 (before → after, C 대응값)

| 경로 (1024B) | Ir/msg before → after | `new`/msg before → after | C 대응 |
|---|---:|---:|---:|
| `DEALER_DEALER` | 13.47k → 12.46k | 3.42 → 1.39 | 9.06k Ir/msg, 0.26 `new`/msg |
| REQREP (op 단위) | 50.71k → 49.16k | 12.57 → 10.44 | 23.98k Ir/op, 1.13 `new`/op |

### binding 상위 항목 (호출 수; 괄호는 msg 또는 op당)

| 항목 | DD before (11,540 msg) | DD after (12,414 msg) | REQREP before (1,336 op) | REQREP after (1,405 op) | 판단 |
|---|---:|---:|---:|---:|---|
| completion entry 생성 | 11,540 (1.00) | 12,414 (1.00) | 1,336 (1.00) | 1,405 (1.00) | completion identity는 필요. result와 묶어 한 번에 할당 |
| owner register / unregister / map emplace | 각 11,540 (1.00) | 각 17 (0.0014) | 각 1,336 (1.00) | 각 1,405 (1.00) | DD 즉시 admission의 map/lock 제거. REQREP completion 대기는 유지하되 map node 재사용 |
| async ready / take | 각 11,540 (1.00) | 각 12,414 (1.00) | 각 1,336 (1.00) | 각 1,405 (1.00) | terminal publish 뒤 mutex 제거 |
| coroutine suspend + scheduler `std::function` | 각 15 (0.0013) | 각 17 (0.0014) | 각 1,336 (1.00) | 각 1,405 (1.00) | DD는 backpressure에서만 발생. REQREP에 남은 op당 고정 비용 |
| `message_t` move constructor | 23,080 (2.00) | 24,828 (2.00) | 6,681 (5.00) | 7,026 (5.00) | payload 복사는 관측되지 않았고 wrapper move만 남음. 공개 ownership 의미 때문에 pass 1에서 유지 |
| 전체 `operator new` | 39,422 (3.42) | 17,318 (1.39) | 16,797 (12.57) | 14,662 (10.44) | bundle과 map-node pool 효과. C: DD 4,696/18,307 = 0.26, REQREP 5,103/4,510 = 1.13 |

판단: DD의 주된 추가 비용은 즉시 성공 메시지마다 붙던 completion map/lock과 분리 allocation이었다.
REQREP는 필수 completion 대기 위에 coroutine scheduler/`std::function`, wrapper move, 남은 allocator
비용이 op마다 겹치는 구조다. 거절 경로 탐색, drain/wake의 전역 scan, payload copy는 상위 추가 비용으로
관측되지 않았다(before log의 개선 후보 1·2 중 §2.1/§2.3 항목이 확인됐고 §2.4 payload 왕복은 해당 없음).

## 변경 (`bindings/cpp/**`만, 공개 헤더 `bindings/cpp/include/zlink` diff 0줄)

| 파일 | 변경 |
|------|------|
| `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp` | async terminal을 release/acquire publish로 전환해 `ready()`와 완료 후 단일-consumer `take()`의 mutex 제거 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp` | async result와 completion entry를 한 control block에 함께 할당하는 bundle과 aliasing `shared_ptr` 선언, map-node pool 선언 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp` | owner mutex 아래 unordered-map node만 socket 수명 PMR pool에서 재사용(completion entry와 Core callback identity는 재사용하지 않음); 선행 `WRITABLE` completion 보존·재생으로 submit과 등록 사이 concurrent drain 처리 |
| `bindings/cpp/src/Runtime/Messaging/send_operations.cpp` | SEND 첫 `DONTWAIT` admission을 owner 등록보다 먼저 수행. 즉시 성공은 owner mutex/map을 건너뛰고 실제 backpressure만 등록; 등록 실패 전까지 source ownership detach를 늦춰 기존 실패 ownership 유지 |
| `bindings/cpp/src/Runtime/Messaging/request_reply.cpp` | REQUEST 경로에 result/entry bundle 할당과 map-node pool 적용 |
| `bindings/cpp/tests/contract/test_cpp_contract_optimization_guard.cpp` | 선행 completion 보존과 submit-before-register 순서를 guard로 고정 |

public interface, ownership, error contract, 측정 의미는 바꾸지 않았다(§5).

## Report

after report: `perf_cpp_multi_linux_20260905_041338.txt` — 작업 worktree 사본
`/home/hep7hep7/project/zlink-wt-cpp-perf/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_041338.txt`
(`status: complete`, `success: 15`, `expected_result_lines: 75`, `actual_result_lines: 75`,
`META,timestamp` 04:13:38 KST, `META,load_avg` 0.77 0.55 1.08).

| Pattern | C report (`bindings/c/perf/results/multi/report/`, before와 동일) | C++ after report |
|---------|------|--------|
| `MULTI_DEALER_DEALER` | `perf_c_multi_linux_20260905_034919_p1cpp.txt` | `perf_cpp_multi_linux_20260905_041338.txt` |
| `MULTI_DEALER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_035123_p1cpp.txt` | 위와 같은 report |
| `MULTI_ROUTER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_035218_p1cpp.txt` | 위와 같은 report |

auto-HWM detail: `MULTI_DEALER_DEALER` after report에서 client 1048576/1048576, server 4096000/4096000
(before C++는 1024B server만 1048576). `MsgUnit(B)`는 여전히 `?`. REQREP report에는 before와 같이
`Auto-HWM detail` 블록이 없다. memory guard cap 기록 없음.

## Before / after / C

비율은 `C++ / C`, 변화는 `after / before − 1`. 처리량 단위는 `DEALER_DEALER` msg/s, 두 REQREP ops/s.
latency는 평균 latency(ms)와 `C++ after / C` 비율이다.

### `MULTI_DEALER_DEALER` (단순 one-way, 목표 평균 95% / 개별 최소 85%)

| Size | C | C++ before | C++ after | 변화 | before/C | after/C | C latency | after latency | latency ratio |
|------|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 1,079,498.4 | 605,522.0 | 740,751.0 | +22.3% | 56.1% | 68.6% | 0.519 ms | 0.321 ms | 0.62x |
| 256 | 974,458.6 | 586,876.2 | 711,017.8 | +21.2% | 60.2% | 73.0% | 2.039 ms | 0.360 ms | 0.18x |
| 1024 | 862,834.2 | 593,257.0 | 722,420.4 | +21.8% | 68.8% | 83.7% | 1.376 ms | 6.786 ms | 4.93x |
| 4096 | 358,081.8 | 292,841.6 | 335,123.0 | +14.4% | 81.8% | 93.6% | 649.123 ms | 757.220 ms | 1.17x |
| 65536 | 77,381.8 | 83,807.2 | 104,544.8 | +24.7% | 108.3% | 135.1% | 19.447 ms | 13.418 ms | 0.69x |

- throughput ratio 산술평균 **75.0% → 90.8%**(중앙값 68.8% → 83.7%) — 기본 목표 95% 미달.
  개별 최소 85% 미달: 64, 256, 1024. 4096, 65536은 개별 최소 통과.
- 평균 latency ratio 산술평균 **0.58x → 1.52x**(중앙값 0.69x) — 2.0x 상한 이내. 1024B가 4.93x로
  개별 상한 초과이며 latency outlier로 기록한다(C 1.376 ms, C++ after 6.786 ms; before C++ 0.524 ms).
  단일 run이라 반복으로 걸러내지 않았고, 두 번째 pass에서 1024B latency를 함께 확인한다.
- §2.1의 완화 목표 90%는 aggregate 90.8%로 수치상 충족하지만, Sol 리뷰 pass가 남아 있으므로
  pass 1에서는 선택하지 않는다.

### `MULTI_DEALER_ROUTER_REQREP` (socket request/reply, 목표 평균 85% / 개별 최소 75%)

| Size | C | C++ before | C++ after | 변화 | before/C | after/C | C latency | after latency | latency ratio |
|------|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 155,527.2 | 63,233.8 | 86,065.0 | +36.1% | 40.7% | 55.3% | 0.963 ms | 0.553 ms | 0.57x |
| 256 | 149,064.2 | 64,462.8 | 70,955.8 | +10.1% | 43.2% | 47.6% | 0.984 ms | 0.689 ms | 0.70x |
| 1024 | 160,067.4 | 60,897.8 | 62,216.8 | +2.2% | 38.0% | 38.9% | 0.990 ms | 0.784 ms | 0.79x |
| 4096 | 128,155.6 | 58,332.0 | 57,051.0 | −2.2% | 45.5% | 44.5% | 1.263 ms | 0.858 ms | 0.68x |
| 65536 | 22,993.0 | 21,033.2 | 21,354.2 | +1.5% | 91.5% | 92.9% | 2.323 ms | 2.294 ms | 0.99x |

- throughput ratio 산술평균 **51.8% → 55.8%**(중앙값 43.2% → 47.6%) — 목표 85% 미달.
  개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.80x → 0.75x**(중앙값 0.70x) — 2.0x 상한 이내.
- 4096B는 단일 after run에서 −2.2%다. 측정 의미를 유지하기 위해 재측정으로 값을 골라내지 않았다.
  두 번째 pass에서 반복 분산과 해당 경로 비용을 함께 확인한다.

### `MULTI_ROUTER_ROUTER_REQREP` (socket request/reply, 목표 평균 85% / 개별 최소 75%)

| Size | C | C++ before | C++ after | 변화 | before/C | after/C | C latency | after latency | latency ratio |
|------|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 139,506.2 | 62,543.8 | 88,867.4 | +42.1% | 44.8% | 63.7% | 0.960 ms | 0.543 ms | 0.57x |
| 256 | 128,620.4 | 58,917.2 | 70,545.8 | +19.7% | 45.8% | 54.8% | 1.078 ms | 0.682 ms | 0.63x |
| 1024 | 117,738.4 | 58,578.6 | 64,666.8 | +10.4% | 49.8% | 54.9% | 1.029 ms | 0.747 ms | 0.73x |
| 4096 | 103,833.8 | 55,796.4 | 59,820.2 | +7.2% | 53.7% | 57.6% | 1.314 ms | 0.807 ms | 0.61x |
| 65536 | 19,822.2 | 21,690.4 | 21,953.8 | +1.2% | 109.4% | 110.8% | 2.617 ms | 2.179 ms | 0.83x |

- throughput ratio 산술평균 **60.7% → 68.4%**(중앙값 49.8% → 57.6%) — 목표 85% 미달.
  개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.77x → 0.67x**(중앙값 0.63x) — 2.0x 상한 이내.

## Gate

| 항목 | 결과 |
|------|------|
| `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 bash bindings/cpp/tests/run_tests.sh` | PASS |
| contract | 16/16 PASS |
| sample smoke | 7/7 PASS |
| `test_cpp_contract_request_reply`, `test_cpp_contract_request_writable_retry`, `test_cpp_perf_application_ready_queue`, `test_cpp_contract_optimization_guard` | 각 5회 반복 PASS |
| `test_cpp_send_close_stress` | PASS (`ownership_failures=0`, `bad_records=0`, `unexpected=0`) |
| `git diff --check` | PASS |
| 공개 API (`bindings/cpp/include/zlink`) | diff 0줄, signature 변경 없음 |
| 대상 외 대표 셀 회귀(§2.2, 처리량 −5% / latency +10%) | 이 pass에서는 대상 3 pattern만 측정했다. 대상 안에서 처리량이 낮아진 셀은 `DEALER_ROUTER_REQREP` 4096B −2.2%(회귀 기준 이내)뿐이고, latency가 높아진 셀은 `DEALER_DEALER` 1024B(0.524 → 6.786 ms)와 4096B(822.232 → 757.220 ms, 감소)로 1024B만 위 outlier에 해당한다. `MULTI_PUBSUB` after는 미측정 |

## 판정 요약 (pass 1 after)

| Pattern | throughput aggregate (before → after) | latency aggregate (before → after) | 상태 |
|---------|---:|---:|------|
| `MULTI_DEALER_DEALER` | 75.0% → 90.8% | 0.58x → 1.52x | `미달(90.8%)`, Sol 리뷰 pass 전 |
| `MULTI_DEALER_ROUTER_REQREP` | 51.8% → 55.8% | 0.80x → 0.75x | `미달(55.8%)`, Sol 리뷰 pass 전 |
| `MULTI_ROUTER_ROUTER_REQREP` | 60.7% → 68.4% | 0.77x → 0.67x | `미달(68.4%)`, Sol 리뷰 pass 전 |
| `MULTI_PUBSUB` | 93.3% (변경 없음) | 1.06x (변경 없음) | `미달(93.3%)`, 개선 pass 전 |

## 남은 gap

- 두 REQREP pattern은 pass 1 뒤에도 C의 39~64%다(`DEALER_ROUTER_REQREP` 256~4096B 38.9~47.6%,
  `ROUTER_ROUTER_REQREP` 64~4096B 54.8~63.7%). 65536B는 두 pattern 모두 92.9~110.8%로 C와 같다.
  pass 1의 allocation 감소만으로는 REQREP에 op마다 남는 protocol/completion 고정 비용(coroutine
  scheduler `std::function`, wrapper move 5회/op, 남은 10.44 `new`/op 대 C 1.13)을 해소하지 못했다.
- `DEALER_ROUTER_REQREP` 4096B는 단일 run에서 −2.2%다. 값을 골라내지 않았고 다음 pass에서 반복
  분산과 해당 경로 비용을 함께 확인한다.
- `DEALER_DEALER`는 90.8%로 기본 목표 95%에 4.2%p 못 미친다. 64~1024B가 68.6~83.7%로 개별 최소 미달이며
  1024B latency outlier(4.93x)가 남아 있다.
- 다음 단계는 §7.4 11단계: Sol read-only 리뷰를 요청하고, 계약 보존 후보로 두 번째 개선 pass를 수행한 뒤
  after를 한 번 측정한다. 안전한 후보가 없으면 그 no-go를 두 번째 pass의 결과로 기록한다.
  이 pass 1 코드 변경은 아직 커밋하지 않았다(작업 worktree의 tracked 변경 6개 파일).
