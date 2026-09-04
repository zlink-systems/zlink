# C++ Multi `tcp` Sol read-only 리뷰 기반 개선 pass 2 — after 측정과 transport 판정 — Core 0.17.0

2026-09-05 04:33~04:35 KST에 §7.4 11~12단계의 두 번째 개선 pass를 수행하고 after를 한 번 측정했다.
pass 1(`log/2026-09-05-cpp-multi-tcp-pass1.ko.md`, 커밋 `86b897abf7`)의 코드를 기준으로 Sol read-only
리뷰가 제시한 후보 9개를 검토해 계약 보존 후보 3개를 채택했다. 대상은 pass 1과 같은 `tcp` 3 pattern
(`MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`)이다. `MULTI_PUBSUB`은
이 pass에서도 다루지 않았고 before 값 그대로다. 이 로그 끝에서 §7.4 15~16단계에 따라 transport별
판정을 닫는다.

## Manifest

| 항목 | 값 |
|------|----|
| source | `main` / `ee50ebaeaf`(pass 1 커밋 `86b897abf7` 포함) + pass 2 작업 tree 변경(`bindings/cpp/src/Runtime/Messaging/**` 5개 파일, 아래 "변경") |
| 작업 worktree | `/home/hep7hep7/project/zlink-wt-cpp-perf2` (job `c016`) |
| Core runtime | `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` (before·pass 1과 동일 artifact; `053a568ddd..ee50ebaeaf`에 `core/` diff 없음) |
| Core build / version / revision | local `Release` + LTO / `0.17.0` / `ee50ebaeaf0ec108571dac92133c90a432c11149` (`core_dirty=0`) |
| binding | `zlink_cpp` 0.17.0, GCC/G++ 13.3.0 |
| host | before·pass 1과 같은 WSL2 host, 같은 boot session (`log/2026-09-05-environment.ko.md`) |
| CPU 상태 | `--pin-cpu` 없음, 동시 perf process 없음, 시작 load average 0.28 0.53 0.83 |
| pair tag | after report는 `--results-tag` 없이 실행했다. paired C는 before의 `p1cpp` C report를 그대로 사용한다(§7.3) |

## Paired C 규칙 적용

- after는 before의 C report(`perf_c_multi_linux_20260905_{034919,035123,035218}_p1cpp.txt`, 03:49~03:52 KST)와
  비교했다. after 실행 시각은 04:35 KST로 C 측정과 43~46분 차이다.
- host boot, Core runtime artifact, 성능 환경이 바뀌지 않았다. `META,core_revision`은 `053a568ddd`에서
  `ee50ebaeaf`로 바뀌었지만 두 revision 사이에 `core/` 변경이 없고 `core_runtime` 경로와 `core_dirty=0`이
  같으므로 같은 Core artifact다. 시작 load average(0.28 0.53 0.83)는 before·pass 1보다 낮았다. §7.3의 C
  재측정 조건에는 해당하지 않아 C를 다시 측정하지 않았다.
- pass 1과 같이 3 pattern을 한 report에 순차 실행했다(pattern cooldown 3000 ms). Effective Options는
  before C++·pass 1 after report와 모두 같다(`clients: 100`, `default_clients: 100`, I/O 4/4, auto-HWM
  balanced, `duration_seconds: 5`, `runs: 1`).

## 명령

```bash
# after (작업 worktree /home/hep7hep7/project/zlink-wt-cpp-perf2 에서 실행)
ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 \
  bash bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern MULTI_DEALER_DEALER,MULTI_DEALER_ROUTER_REQREP,MULTI_ROUTER_ROUTER_REQREP \
  --transports tcp --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1
```

## Sol read-only 리뷰 후보와 채택 여부

리뷰 조건은 §5·§7.4 10단계와 같다. public interface, ownership, error contract, 측정 의미를 유지하는
후보만 채택한다. 리뷰는 코드 변경 없이 pass 1 tree를 대상으로 했다.

| 후보 | 계약 보존 근거 | 예상 효과 | 채택 여부 |
|---|---|---|---|
| reply가 이미 도착한 경우 coroutine suspend 생략 | `async_result_t::awaiter_t::await_ready()`와 `async_operation_state_t::suspend()`의 terminal 재검사가 submit/reply race 양쪽을 이미 처리한다 | 이미 적용되어 추가 효과 없음 | 미채택: 중복 |
| scheduler `std::function`을 함수 포인터/고정 task로 변경 | public header의 `async_continuation_scheduler_t`와 custom coroutine promise 연동 signature/ABI를 바꿔야 한다 | indirect call·manager 비용 감소 가능 | 미채택: 공개 signature 보존 |
| resume slot을 operation bundle 내부에 배치 | operation마다 고유한 bundle/slot identity를 유지한다. queued callback은 aliasing `shared_ptr`로 같은 control block을 잡고, abandon 경쟁은 기존 atomic slot이 처리한다. scheduler의 public `std::function` signature는 유지한다 | suspended operation당 별도 control-block allocation 1회 제거 | **채택** |
| 2-part request staging inline화 | native submit은 이미 8-part stack staging을 사용하고, builder vector capacity는 pooled operation state에서 재사용한다. callgrind에서 state allocation은 1,405 op 중 1회였다 | 정상 상태에서 줄일 allocation 없음 | 미채택: 중복·복잡도 증가 |
| reply `message_t`를 vector 원소에 직접 adopt | 반환 `std::vector<message_t>`, part 순서, native ownership은 그대로이고 임시 wrapper→vector 이동만 없앤다 | 2-part reply에서 move 2회/op 제거 | **채택** |
| socket당 첫 completion entry를 inline 보관 | 첫 entry도 기존 owner mutex가 소유하며 callback identity를 재사용하지 않는다. 둘째 이후 동시 operation은 기존 PMR map을 그대로 사용한다 | 흔한 1-outstanding 경로의 hash/node register·lookup·erase 제거 | **채택** |
| completion map node pool 확대 또는 entry/Future pool | PMR node는 이미 socket lifetime 동안 재사용한다. entry/Future identity 재사용은 늦은 completion의 ABA 위험이 있어 가이드 §4 금지 대상이다 | 추가 allocation 감소 근거 없음 | 미채택: no-go |
| perf REQREP client의 part/copy 방식 변경 | C와 C++ 모두 client당 1 outstanding, 같은 payload 크기, 같은 1/2 part 설정, payload→native message 복사 1회를 사용한다 | parity를 지키며 없앨 차이 없음 | 미채택: 측정 의미 보존(§5) |
| public wrapper/Future 또는 coroutine frame pool | public consumer identity와 늦은 completion 수명이 관찰되며 가이드 §4의 pool 금지 범위에 해당한다 | allocation 감소 가능 | 미채택: ABA·ownership 위험 |

### No-go 기록 (§7.4 11·16단계 근거)

- 이미 존재하는 await-ready/recheck fast path를 중복 구현하지 않았다.
- scheduler type을 바꾸지 않았다. 이를 없애려면 public header ABI와 custom promise 계약이 달라진다
  (§7.5: public contract 변경이 필요한 후보는 우회 구현으로 통과시키지 않는다).
- 2-part native staging과 PMR map-node 재사용은 이미 적용돼 있어 별도 inline buffer/pool을 추가하지 않았다.
- entry/Future/public wrapper/coroutine frame을 재사용하지 않았다. 늦은 completion과 새 operation 사이의
  ABA를 만들 수 있다.
- C와 part 수·copy 수가 같은 perf client는 바꾸지 않았다. in-flight 상한, poll 방식, 측정 deadline도 그대로다.
- 단일 공식 after의 하락 셀을 지우기 위한 재측정이나 결과 선택을 하지 않았다(§7.4 12단계).

## 변경 (`bindings/cpp/src/Runtime/Messaging/**`만, 공개 헤더 `bindings/cpp/include/zlink` diff 0줄)

| 파일 | 변경 |
|------|------|
| `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp` | public async operation의 resume slot을 고유 completion bundle 안에 두고, queued callback의 수명은 aliasing `shared_ptr`로 보존. standalone internal state는 기존 별도 slot allocation fallback 유지 |
| `bindings/cpp/src/Runtime/Messaging/request_reply.cpp`, `send_operations.cpp` | bundle 생성 직후 result에 bundle lifetime을 묶음. 공개 terminal과 error mapping은 변경 없음 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp`, `completion_owner.cpp` | socket당 첫 completion entry는 inline `shared_ptr`에 두고 추가 동시 entry만 기존 PMR map에 둠. shutdown·public/runtime owner 전환·send entry count 조건은 두 저장소를 함께 확인 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp` | reply vector를 먼저 resize하고 native reply part를 원소에 직접 adopt |

public interface, ownership, error contract, 측정 의미는 바꾸지 않았다(§5). 변경은 작업 worktree
`/home/hep7hep7/project/zlink-wt-cpp-perf2`에 미커밋 상태다.

## 비용 위치 확인 (callgrind)

조건은 pass 1과 같은 10 clients, `tcp`, 1024B, 1초 `MULTI_DEALER_ROUTER_REQREP`다(pass 1 로그가 확인하기로
한 REQREP callgrind pattern은 `MULTI_DEALER_ROUTER_REQREP`로 확정). 처리량 판정에는 쓰지 않는다. pass 2 run은
deadline 뒤 정상 drain된 10건을 포함해 submit/completion 1,415건으로 정규화했다.
report(`/home/hep7hep7/project/zlink-wt-cpp-perf2/bindings/cpp/perf/results/multi/report/`):
`perf_cpp_multi_linux_20260905_043334_cpp_pass2_diag.txt`(diag),
`perf_cpp_multi_linux_20260905_043356_cpp_pass2_callgrind_rr.txt`(callgrind).
profile: `/home/hep7hep7/project/zlink-work/c016/profiles/cpp-reqrep-pass2.callgrind`.

| 항목 | pass 1 after | pass 2 after | 변화 |
|---|---:|---:|---:|
| 전체 Ir | 69,077,105 / 1,405 op | 68,721,654 / 1,415 op | −0.5% total(작업량 차이 포함) |
| Ir/op | 49.16k | 48.57k | −1.2% (C 23.98k) |
| 전체 `operator new` | 14,662 / 1,405 op | 13,336 / 1,415 op | 작업량 차이 포함 |
| `new`/op | 10.44 | 9.42 | −9.8% (C 1.13) |
| `message_t` move/op | 5.00 | 3.00 | −40.0% |
| 별도 `async_resume_slot_t` allocation | 1.00/op | 0/public operation | 제거 |

판단: 채택한 3개 후보는 op당 allocation 1회, wrapper move 2회, 1-outstanding 경로의 map register/lookup/erase를
없앴지만 REQREP op당 고정비는 여전히 C의 약 2배(48.57k 대 23.98k Ir/op)다. 남은 비용의 주된 항목은 coroutine
suspend/resume과 public `std::function` scheduler, 필수 completion 대기 구조로, 위 no-go 목록처럼 public
header signature 또는 ownership 계약을 바꾸지 않고는 제거할 수 없다.

## Report

after report: `perf_cpp_multi_linux_20260905_043517.txt` — 작업 worktree 사본
`/home/hep7hep7/project/zlink-wt-cpp-perf2/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_043517.txt`
(`status: complete`, `success: 15`, `expected_result_lines: 75`, `actual_result_lines: 75`,
`META,timestamp` 04:35:17 KST, `META,load_avg` 0.28 0.53 0.83).

| Pattern | C report (`bindings/c/perf/results/multi/report/`, before와 동일) | C++ pass 2 after report |
|---------|------|--------|
| `MULTI_DEALER_DEALER` | `perf_c_multi_linux_20260905_034919_p1cpp.txt` | `perf_cpp_multi_linux_20260905_043517.txt` |
| `MULTI_DEALER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_035123_p1cpp.txt` | 위와 같은 report |
| `MULTI_ROUTER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_035218_p1cpp.txt` | 위와 같은 report |

auto-HWM detail: `MULTI_DEALER_DEALER`는 pass 1 after와 같이 client 1048576/1048576, server 4096000/4096000.
`MsgUnit(B)`는 여전히 `?`. REQREP report에는 `Auto-HWM detail` 블록이 없다. memory guard cap 기록 없음.

## pass 1 / pass 2 / C

비율은 `C++ / C`, 변화는 `pass 2 / pass 1 − 1`. 처리량 단위는 `DEALER_DEALER` msg/s, 두 REQREP ops/s.
latency는 평균 latency(ms)와 `C++ pass 2 / C` 비율이다. C와 pass 1 값은 pass 1 로그와 같다.

### `MULTI_DEALER_DEALER` (단순 one-way, 기본 목표 평균 95% / 완화 목표 90% / 개별 최소 85%)

| Size | C | C++ pass 1 | C++ pass 2 | 변화 | pass 1/C | pass 2/C | C latency | pass 1 latency | pass 2 latency | latency ratio |
|------|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 1,079,498.4 | 740,751.0 | 743,440.6 | +0.36% | 68.6% | 68.9% | 0.519 ms | 0.321 ms | 0.207 ms | 0.40x |
| 256 | 974,458.6 | 711,017.8 | 724,345.8 | +1.87% | 73.0% | 74.3% | 2.039 ms | 0.360 ms | 0.286 ms | 0.14x |
| 1024 | 862,834.2 | 722,420.4 | 709,697.4 | −1.76% | 83.7% | 82.3% | 1.376 ms | 6.786 ms | 0.903 ms | 0.66x |
| 4096 | 358,081.8 | 335,123.0 | 331,874.4 | −0.97% | 93.6% | 92.7% | 649.123 ms | 757.220 ms | 773.400 ms | 1.19x |
| 65536 | 77,381.8 | 104,544.8 | 105,103.2 | +0.53% | 135.1% | 135.8% | 19.447 ms | 13.418 ms | 13.437 ms | 0.69x |

- throughput ratio 산술평균 **90.8% → 90.8%**(pass 1 대비 size 평균 +0.01%, 중앙값 83.7% → 82.3%). 채택 후보는
  REQREP completion 대기 경로 대상이라 DD의 즉시 admission 경로에는 영향이 없고, 변화는 단일 run 편차 범위다.
- 평균 latency ratio 산술평균 **1.52x → 0.62x**(중앙값 0.66x) — 2.0x 상한 이내. pass 1의 1024B 4.93x outlier는
  pass 2 단일 run에서 0.66x(0.903 ms)로 재현되지 않았다.
- 개별 최소 85% 미달: 64(68.9%), 256(74.3%), 1024(82.3%). 4096, 65536은 개별 최소 통과.

### `MULTI_DEALER_ROUTER_REQREP` (socket request/reply, 목표 평균 85% / 개별 최소 75%)

| Size | C | C++ pass 1 | C++ pass 2 | 변화 | pass 1/C | pass 2/C | C latency | pass 1 latency | pass 2 latency | latency ratio |
|------|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 155,527.2 | 86,065.0 | 80,275.0 | −6.73% | 55.3% | 51.6% | 0.963 ms | 0.553 ms | 0.609 ms | 0.63x |
| 256 | 149,064.2 | 70,955.8 | 81,202.8 | +14.44% | 47.6% | 54.5% | 0.984 ms | 0.689 ms | 0.599 ms | 0.61x |
| 1024 | 160,067.4 | 62,216.8 | 73,514.8 | +18.16% | 38.9% | 45.9% | 0.990 ms | 0.784 ms | 0.665 ms | 0.67x |
| 4096 | 128,155.6 | 57,051.0 | 58,162.8 | +1.95% | 44.5% | 45.4% | 1.263 ms | 0.858 ms | 0.841 ms | 0.67x |
| 65536 | 22,993.0 | 21,354.2 | 20,585.0 | −3.60% | 92.9% | 89.5% | 2.323 ms | 2.294 ms | 2.375 ms | 1.02x |

- throughput ratio 산술평균 **55.8% → 57.4%**(pass 1 대비 size 평균 +4.84%, 중앙값 47.6% → 51.6%) — 목표 85% 미달.
  5% 효과 기준을 넘은 셀은 256B(+14.44%)와 1024B(+18.16%)다. 64B −6.73%, 65536B −3.60%는 단일 run이며
  이번 변경은 size별 분기를 추가하지 않았고 callgrind 고정비가 감소했으므로 값을 골라내지 않고 그대로 남긴다.
  pass 1의 4096B −2.2%는 pass 2에서 +1.95%다.
- 평균 latency ratio 산술평균 **0.75x → 0.72x**(중앙값 0.67x) — 2.0x 상한 이내.
- 개별 최소 75% 미달: 64, 256, 1024, 4096.

### `MULTI_ROUTER_ROUTER_REQREP` (socket request/reply, 목표 평균 85% / 개별 최소 75%)

| Size | C | C++ pass 1 | C++ pass 2 | 변화 | pass 1/C | pass 2/C | C latency | pass 1 latency | pass 2 latency | latency ratio |
|------|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 139,506.2 | 88,867.4 | 89,505.0 | +0.72% | 63.7% | 64.2% | 0.960 ms | 0.543 ms | 0.542 ms | 0.56x |
| 256 | 128,620.4 | 70,545.8 | 61,315.4 | −13.08% | 54.8% | 47.7% | 1.078 ms | 0.682 ms | 0.790 ms | 0.73x |
| 1024 | 117,738.4 | 64,666.8 | 66,712.0 | +3.16% | 54.9% | 56.7% | 1.029 ms | 0.747 ms | 0.728 ms | 0.71x |
| 4096 | 103,833.8 | 59,820.2 | 61,991.0 | +3.63% | 57.6% | 59.7% | 1.314 ms | 0.807 ms | 0.782 ms | 0.60x |
| 65536 | 19,822.2 | 21,953.8 | 22,588.6 | +2.89% | 110.8% | 114.0% | 2.617 ms | 2.179 ms | 2.100 ms | 0.80x |

- throughput ratio 산술평균 **68.4% → 68.4%**(pass 1 대비 size 평균 −0.54%, 중앙값 57.6% → 59.7%) — 목표 85% 미달.
  256B −13.08%는 단일 run이며 위와 같은 이유로 값을 골라내지 않고 그대로 남긴다. 나머지 4 size는 +0.72~+3.63%.
- 평균 latency ratio 산술평균 **0.67x → 0.68x**(중앙값 0.71x) — 2.0x 상한 이내.
- 개별 최소 75% 미달: 64, 256, 1024, 4096.

## Gate

| 항목 | 결과 |
|------|------|
| `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 bash bindings/cpp/tests/run_tests.sh` | PASS |
| contract | 16/16 PASS |
| sample smoke | 7/7 PASS |
| `test_cpp_contract_request_reply`, `test_cpp_contract_request_writable_retry`, `test_cpp_perf_application_ready_queue`, `test_cpp_contract_optimization_guard` | 각 5회 반복 PASS |
| `test_cpp_send_close_stress` | PASS |
| `git diff --check` | PASS |
| 공개 API (`bindings/cpp/include/zlink`) | diff 0줄, signature 변경 없음 |
| Core | 기존 untracked `core/build`, `core/build-dev` symlink 유지, Core configure/build/clean 없음 |
| 대상 외 대표 셀 회귀(§2.2, 처리량 −5% / latency +10%) | 이 pass도 대상 3 pattern만 측정했다. 대상 안에서 −5%를 넘게 낮아진 셀은 `DEALER_ROUTER_REQREP` 64B(−6.73%)와 `ROUTER_ROUTER_REQREP` 256B(−13.08%), latency가 +10% 넘게 높아진 셀은 같은 두 셀(+10.1%, +15.8%)이다. 두 셀 모두 단일 run이고 변경에 size별 분기가 없으며 같은 pattern의 인접 size는 개선됐으므로 회귀가 아닌 편차로 기록하되, 정책에 따라 재측정으로 걸러내지 않았다. `MULTI_PUBSUB` after는 미측정 |

## 판정 (§7.4 12·15·16단계, §8)

두 개선 pass(자체 pass 1, Sol 리뷰 pass 2)를 모두 마쳤으므로 transport 판정을 닫는다. 모든 size report는
complete이고 latency aggregate는 세 pattern 모두 2.0x 이내다.

| Pattern | throughput aggregate (before → pass 1 → pass 2) | latency aggregate (pass 2) | 적용 목표 | 상태 |
|---------|---:|---:|---|------|
| `MULTI_DEALER_DEALER` | 75.0% → 90.8% → 90.8% | 0.62x | 단순 one-way 완화 목표 90%(§2.1) | `통과(90.8%)` |
| `MULTI_DEALER_ROUTER_REQREP` | 51.8% → 55.8% → 57.4% | 0.72x | socket request/reply 85% | `보류(57.4%)` |
| `MULTI_ROUTER_ROUTER_REQREP` | 60.7% → 68.4% → 68.4% | 0.68x | socket request/reply 85% | `보류(68.4%)` |
| `MULTI_PUBSUB` | 93.3% (변경 없음) | 1.06x (변경 없음) | 단순 one-way 기본 목표 95% | `미달(93.3%)`, 개선 pass 전 |

### `MULTI_DEALER_DEALER` — 완화 목표 90% 선택과 `통과(90.8%)`

- §2.1은 C++ 단순 one-way의 기본 목표를 95%로 두되, 이를 맞추기 위한 개선 작업이 과도하게 길어지면
  현재 작업에서만 90%를 완화 목표로 선택할 수 있고 그 경우에도 size 평균 90%를 달성해야 한다고 정한다.
  이 transport에 대해 완화 목표 90%를 선택한다.
- 근거: 자체 pass 1(+15.8%p)과 Sol 리뷰 pass 2를 모두 수행했다. pass 1은 DD의 즉시 admission 경로에서
  map/lock과 분리 allocation을 제거했고(1024B `new`/msg 3.42 → 1.39, C 0.26), pass 2의 Sol 리뷰는 DD 경로에
  추가로 적용할 계약 보존 후보를 제시하지 않았다(채택 3개는 completion 대기 경로 대상이며 DD 변화 +0.01%).
  남은 후보(scheduler type 변경, wrapper/Future pool)는 public signature 또는 ownership 계약 변경이 필요해
  §7.5에 따라 채택하지 않는다. 즉 공개 contract를 유지한 채 95%에 이르는 후보가 없고, size 평균 90.8%는
  완화 목표 90%를 충족한다.
- 개별 최소 85% 미달 outlier: 64B 68.9%, 256B 74.3%, 1024B 82.3%. §2.1에 따라 aggregate가 목표를 충족하므로
  개별 값만으로 미달로 바꾸지 않고 측정 기록으로 남긴다. latency 0.62x는 2.0x 이내이고 pass 1의 1024B
  outlier는 재현되지 않았다.
- §7.4 15단계: 채택한 pass 2 변경은 검증 뒤 커밋·푸시하고 다음 transport로 이동한다(아직 미커밋).

### `MULTI_DEALER_ROUTER_REQREP`·`MULTI_ROUTER_ROUTER_REQREP` — `보류` 근거

- §7.4 16단계와 §8의 `보류` 조건: paired 측정, 자체 개선 pass, Sol 리뷰 기반 두 번째 개선 pass를 완료했고,
  public contract를 유지한 추가 개선 요소가 없다. 세 조건을 모두 만족한다.
  - paired 측정: `p1cpp` C 직후 C++ before, pass 1 after, pass 2 after 각 1회, 모두 `status: complete`.
  - 자체 pass 1: result/entry bundle, map-node PMR pool, async terminal lock-free publish — 51.8→55.8%,
    60.7→68.4%.
  - Sol 리뷰 pass 2: 채택 3개(bundle 내부 resume slot, reply 직접 adopt, inline 첫 completion entry) —
    55.8→57.4%, 68.4→68.4%. callgrind `new`/op 10.44→9.42, move/op 5→3이지만 Ir/op는 48.57k로 C(23.98k)의 2배.
  - 남은 후보는 모두 no-go다: scheduler `std::function` 제거는 public header ABI·custom promise 계약 변경,
    entry/Future/wrapper/coroutine frame pool은 늦은 completion ABA·ownership 위험, perf client 변경은 측정
    의미 훼손, 나머지는 이미 적용된 fast path·staging·pool의 중복이다.
- 변동 폭이나 안정성을 이유로 한 `보류`가 아니다. 두 pattern은 pass 1·pass 2 모두 64~4096B가 개별 최소
  75% 미달(45.4~64.2%)이고 65536B만 89.5~114.0%로 C와 같다. 남은 격차는 REQREP op당 필수 completion 대기 위의
  coroutine/scheduler/wrapper 고정비로, 공개 async 계약(`async_continuation_scheduler_t`, `message_t`
  ownership, Future identity)을 유지하는 한 제거 대상이 없다.
- 단일 run 하락 셀(`DEALER_ROUTER_REQREP` 64B −6.73%, `ROUTER_ROUTER_REQREP` 256B −13.08%)은 §7.4 12단계에
  따라 재측정하지 않았고 그대로 기록한다. 이 셀을 pass 1 값으로 바꿔도 aggregate는 58.1% / 69.8%로 목표 85%에
  못 미쳐 판정은 달라지지 않는다.
- 이후 public contract 변경을 동반하는 개선(예: scheduler 계약 재설계)은 이 계획의 범위 밖이며, 별도 계약
  변경 계획에서 다룬다.

### `MULTI_PUBSUB` — 적용 목표와 상태

- `MULTI_PUBSUB`은 §2.1 pattern 그룹 표에서 단순 one-way이므로 aggregate gate는 중앙값 목표 95%다.
  85%는 개별 셀 최소 기준이며 aggregate 판정에는 쓰지 않는다. before 93.3%는 95%에 미달이다.
- 완화 목표 90%는 "개선 작업이 과도하게 길어지는 경우 현재 작업에서만" 선택할 수 있는데, `MULTI_PUBSUB`은
  아직 자체 pass 1도 Sol 리뷰 pass 2도 수행하지 않았으므로 선택 근거가 없다. §7.4 10~11단계의 두 pass가 남아
  있어 `보류`로도 닫을 수 없다. 상태는 `미달(93.3%)`을 유지하고, 다음 작업으로 `MULTI_PUBSUB` 자체 hot-path
  pass와 Sol 리뷰 pass를 수행한다.

## 남은 작업

- pass 2 코드 변경(`bindings/cpp/src/Runtime/Messaging/**` 5개 파일)을 §7.6에 따라 검증 범위만 커밋하고
  푸시한다(§7.4 15단계). 이 문서 갱신 시점에는 미커밋이다.
- `MULTI_PUBSUB` `tcp` 자체 hot-path pass 1과 Sol 리뷰 pass 2, after 측정(§7.4 10~12단계).
- `tcp`의 `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_SENDSEND`, `MULTI_STREAM`, 그리고 `131072`
  size는 미측정이다.
