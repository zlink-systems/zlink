# C++ Multi `tcp` `MULTI_DEALER_DEALER` hot-path pass — Core 0.17.1 — 2026-09-07

> 범위: `bindings/cpp/src`·`bindings/cpp/tests`만 수정했다. perf 러너(`bindings/cpp/perf/**`),
> `bindings/c/**`, `core/**`, `framework/**`, 정책 문서, 계획서, `decisions.ko.md`,
> 다른 언어 binding은 **수정하지 않았다**. Core artifact는
> `core/build/lib/libzlink.so.0.17.1`(`core_revision 074d2a5964`, `core_dirty=0`) 고정,
> 재빌드 없음. 커밋·푸시하지 않았다(감독자 담당).

---

## 0. Manifest

| 항목 | 값 |
|------|----|
| 대상 | C++ Multi suite / `MULTI_DEALER_DEALER` / transport `tcp` |
| host | Intel Core Ultra 7 265K, 20 cores, WSL2 (`Linux 6.6.87.2-microsoft-standard-WSL2`) |
| Core runtime | `core/build/lib/libzlink.so.0.17.1`, `core_version 0.17.1`, `core_revision 074d2a596470dcf2899cd3060858cdd8056ac12e`, `core_dirty=0` |
| binding | `zlink_cpp` 0.17.1, GCC/G++ 13.3.0, Release |
| 측정 조건 | `--pattern MULTI_DEALER_DEALER --transports tcp --msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 1`, `clients 100`(기본값), `ZLINK_CORE_SOURCE=local` |
| 시작 load average | before 측정 시각(10:42~10:43) `0.96 / 1.48`, after 측정 시각(11:21) `1.22`, 확인 run(11:22) `1.08`. 전 구간 5 미만 |
| 동시 perf process | 매 실행 직전 `ps`로 0건 확인, 전부 직렬 실행 |

### 사용한 report

| 용도 | report |
|------|--------|
| C 기준(paired `p2cpp`) | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_104232_p2cpp.txt` |
| C++ before | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_104306_p2cpp.txt` |
| **C++ after (공식)** | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_112120_p2cpp_after.txt` |
| C++ after 확인 run | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_112228_p2cpp_after_confirm.txt` |
| 회귀 gate C 기준 | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_112314_p2cpp_reg.txt` |
| 회귀 gate C++ after | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_112339_p2cpp_reg_after.txt` |
| 회귀 gate C++ before(변경 되돌린 build) | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_112448_p2cpp_reg_before.txt` |

진단용 축소 셀 report(판정에 쓰지 않음): `perf_cpp_multi_linux_20260907_104845_diag_probe.txt`,
`..._104906_diag_probe2.txt`, `..._104942_cg_cpp_client_64.txt`,
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_104853_diag_probe.txt`,
`..._104957_cg_c_client_64.txt`.

### 측정 환경에서 이 pass 도중 바뀐 것 (기록)

이 pass 진행 중(11:13~11:14) **다른 작업이 같은 작업 tree에서 C++ perf 러너의 시간원을 고쳤다**:
`bindings/cpp/perf/multi/common/perf_metric_header.hpp`와 `.../single/common/perf_single_metric_header.hpp`의
`now_ns()`가 `system_clock` → `steady_clock`으로 바뀌었다(`PERF_POLICY.md` §1.1 monotonic 요구).
내 after build에는 이 변경이 포함되고 10:43의 before report에는 포함되지 않는다.

- **처리량 비교에는 영향이 없다.** 수신 측 집계는 metric header의 magic/run/phase/size로만
  판정하며(`perf_metric::is_expected`) `sent_ts_ns`를 쓰지 않는다.
- **평균 latency는 의미가 달라졌다.** before는 client·server가 모두 `system_clock`(WSL2에서 벽시계
  점프가 관측되는 시간원), after는 모두 `steady_clock`이다. 아래 latency 표는 이 사실을 전제로 읽어야 한다.
- 이 변경은 내가 하지 않았고 되돌리지도 않았다. 감독자 판단 항목(§8-2)에 남긴다.

---

## 1. before (재확인)

| Size | C `p2cpp` (msg/s) | C++ before (msg/s) | before/C | C latency(ms) | C++ latency(ms) | latency ratio |
|------|---:|---:|---:|---:|---:|---:|
| 64 | 1,682,102.0 | 1,151,636.0 | 68.46% | 0.062 | 0.064 | 1.03x |
| 256 | 1,533,516.0 | 1,080,648.0 | 70.47% | 1.236 | 0.171 | 0.14x |
| 1024 | 1,224,382.0 | 1,142,451.0 | 93.31% | 0.700 | 0.199 | 0.28x |
| 4096 | 640,748.0 | 618,912.0 | 96.59% | 767.687 | 862.788 | 1.12x |
| 65536 | 161,487.0 | 88,712.0 | 54.93% | 6.790 | 14.011 | 2.06x |

throughput aggregate(비율 산술평균) **76.75%** — 기본 목표 95%, 완화 목표 90% 모두 미달.
latency aggregate **0.93x** — 2.0x 이내 통과.

---

## 2. 진단

### 2.1 진단 harness (공식 러너와 등가임을 확인한 축소 셀)

공식 러너는 size마다 build → server/client 프로세스 쌍을 새로 띄우므로 callgrind·`strace`·CPU 계측을
끼워 넣기 어렵다. 그래서 같은 바이너리(`comp_src_dealer_dealer_{server,client}`,
`cpp_comp_src_dealer_dealer_{server,client}`)를 같은 env(`PERF_MSG_SIZES`, `PERF_MULTI_CLIENTS`,
`PERF_MULTI_DURATION_SECONDS`, `PERF_IO_THREADS=4`, `PERF_CONNECT_CONCURRENCY=128`)로 직접 구동하는
진단 스크립트를 scratchpad에 만들어 썼다(저장소에 파일을 추가하지 않았다). 러너 코드는 손대지 않았다.

등가 확인(clients 10, duration 2):

| | 공식 러너 | 진단 harness |
|---|---:|---:|
| C 64B | 1,676,302 | 1,713,433 / 1,734,377 |
| C++ 64B | 1,167,832 | 1,180,397 / 1,151,550 |
| C 65536B | 200,892 | 194,474 / 244,692 |

64B는 ±3% 이내로 재현된다. 65536B는 진단 harness와 공식 러너 모두 편차가 크다(§2.5).
**모든 판정 수치는 공식 러너 report만 사용했고, 아래 진단 수치는 위치 확인용이다.**

### 2.2 송신 경로가 전부다 — 교차 pairing

DD의 처리량은 **server(수신)** 가 집계한다(`perf_dealer_dealer_server.cpp`, C도 동일). 그래서
C/C++ server·client를 교차로 붙여 어느 쪽이 한계인지 먼저 갈랐다(진단 harness, clients 10, duration 2~3):

| pairing | 64B (msg/s) | 65536B (msg/s) |
|---|---:|---:|
| C server + C client | 1,734,377 | 244,692 |
| **C++ server + C client** | **1,663,543 (95.9%)** | **195,661 (80.0%)** |
| C++ server + C++ client | 1,151,550 (66.4%) | 90,226 (36.9%) |

**C++ 수신 경로는 병목이 아니다.** C client를 붙이면 C++ server는 C server와 거의 같은 속도를 낸다.
두 size 모두 격차는 **C++ client의 송신 경로**에서 나온다. 이후 진단은 client만 대상으로 했다.

### 2.3 callgrind — 64B/256B: 메시지당 고정 비용

조건: clients 4, duration 3, `tcp`, client만 `valgrind --tool=callgrind --cache-sim=no`.
메시지 수는 server RESULT × duration으로 정규화했다.
profile: scratchpad `cg/{cpp_cli_64,c_cli_64,cpp_cli_65536,c_cli_65536,cpp_cli_64_after}.out`.

| 항목 (64B) | C client | C++ client (before) | C++ / C |
|---|---:|---:|---:|
| 전체 Ir / 메시지 | 6,743 | 11,830 | 1.75x |
| application thread 포함 Ir / 메시지 | 5,221 (`run_single_size_case`) | 10,056 (`run_sender`) | 1.93x |
| 그 중 `zlink_send_part` 포함 Ir / 메시지 | 4,498 | 5,105 | 1.13x |
| `operator new` / send | ≈0.018 (2,521건 전체) | **1.03** (82,041 / 79,823 send) | — |
| `zlink_msg_copy` / send | 0 | 2.0 (borrowed native view, part당 1회) | — |
| backpressure(`register_send_entry`) / send | — | **21 / 79,823 = 0.03%** | — |

판단: 64B에서 **backpressure는 사실상 발생하지 않는다(0.03%)**. 그런데도 C++는 send마다
`std::make_shared<send_completion_bundle_t>`로 **completion entry + async operation state를 한 덩어리로 힙 할당**하고
있었다. 그 bundle에는 `std::mutex` + `std::condition_variable` + `std::vector<message_t>` +
`std::exception_ptr` + `std::function` scheduler가 들어 있어, 즉시 admission된 send 한 건마다
할당/해제 · mutex 4쌍 · `pthread_cond_broadcast` 1회 · `pthread_cond_destroy` 1회가 붙었다.

before profile의 self 비용(메시지당 환산):
`send_submit_operation_t::async()` 235, `completion_entry_t::submit_send_attempt` 203,
`~completion_entry_t` 170, `async_operation_state_t<void>::take` 48, bundle `_M_dispose` 46,
`completion_entry_t` ctor 31, `start_send` 28, `pthread_cond_broadcast` 29, `pthread_cond_destroy` 20,
binding 몫 mutex lock/unlock ≈ 190, binding 몫 allocator ≈ 130.

이것은 `BINDINGS_OPTIMIZATION_GUIDE.ko.md` §2.1 첫 줄
("completion entry, Promise/Future/Task, waiter map 등록을 만들지 않는다. 토큰이 반환된 뒤에만 만든다")
가 C++에만 아직 적용되지 않은 상태다. pass 1(`86b897abf7`)은 **map/lock 등록만** 제거했고
entry·result 할당 자체는 성공 경로에도 남아 있었다.

### 2.4 65536B: client가 CPU-bound가 아니다

`/usr/bin/time`로 client 프로세스 전체 CPU를 쟀다(clients 10, duration 3):

| | user(s) | sys(s) | cpu | 메시지 | CPU µs/msg |
|---|---:|---:|---:|---:|---:|
| C client 65536 | 2.99 / 3.23 | 5.35 / 6.17 | 276% / 312% | 595k / 680k | 14.0 |
| C++ client 65536 (before) | 0.87 / 0.90 / 1.18 | 5.56 / 5.74 / 5.55 | 213~223% | 271k | 23.7 |
| C client 64 | 4.29 | 1.63 | 196% | 5,105k | — |
| C++ client 64 (before) | 4.10 | 1.82 | 196% | 3,485k | — |

- 64B: 두 client의 총 CPU가 사실상 같은데(5.92 core-s) C++가 68%의 메시지만 보낸다 →
  **application thread가 포화된 CPU-bound**. §2.3의 고정 비용이 그대로 처리량 손실이다.
- 65536B: C++ client의 **user 시간은 오히려 C보다 작다**(3.2 vs 5.0 µs/msg — C 러너는 메시지마다
  64 KiB `memcpy`를 한 번 더 한다). 총 CPU도 2.1 core 수준으로 20-core host에서 포화가 아니다.
  즉 65536B의 C++ client는 **CPU가 아니라 credit/WRITABLE 대기로 막혀 있었다.**
- 같은 크기에서 backpressure 비율은 **12.5%** (`register_send_entry` 3,391 / 27,169 send)로 64B의
  0.03%와 완전히 다른 영역이다(auto-HWM 1,048,576 B 기준으로 64 KiB는 socket당 16건).
- `strace -c -f`(양쪽 모두 ~12k msg/s로 throttling된 상태)와 `/proc/<pid>/io`(native 속도)에서
  **메시지당 syscall 패턴은 C와 C++가 같았다**: `sendto` 2.01/msg 동일, native 속도의 wakeup
  write 0.25/msg 동일, read 1.34 vs 1.44. 즉 전송 단위가 잘게 쪼개지는 문제는 아니다.

결론: 65536B의 원인은 "메시지당 CPU"가 아니라 **backpressure → WRITABLE → 재제출 → coroutine 재개
경로의 회전 비용**이다. 이 경로도 send 한 건마다 bundle 할당·mutex·cv를 쓰고 있었고, 100 socket ×
12.5% backpressure에서 그 비용이 io thread와 allocator·lock을 다투게 된다.

### 2.5 65536B의 이봉(bimodal) 거동 — 기록

before build에서 같은 조건(진단 harness, clients 10)을 반복하면 65536B가 ~90k에 머무는 run과
~133k가 나오는 run 두 갈래로 갈렸다(90.2 / 90.5 / 91.0 / 91.5 / 89.3 / 91.2 k, 그리고 133.8 / 139.2 k).
공식 러너(clients 100)의 before는 88.7k로 낮은 쪽 값이다. after에서는 두 공식 run이 각각 179.6k,
182.1k로 모두 높은 쪽에 안정적으로 들어왔다. 값을 고르지 않고 공식 run 두 개를 모두 기록한다.

---

## 3. 검토한 후보와 채택/기각

먼저 `BINDINGS_OPTIMIZATION_GUIDE.ko.md` §4와 `decisions.ko.md` D-B121~D-B130,
`log/2026-09-05-cpp-multi-tcp-pass1.ko.md`·`pass2.ko.md`의 no-go 목록을 확인하고, 그 목록에 있는
후보는 다시 제안하지 않았다.

| # | 후보 | 계약 검토 | 판정 |
|---|---|---|---|
| 1 | **즉시 admission된 async SEND에서 completion bundle(entry + async state)과 waiter map 등록을 만들지 않는다** | 가이드 §2.1의 첫 규칙. Core는 record를 거절할 때만 wait token을 등록하므로, 즉시 admission에는 completion identity가 필요 없다. 공개 `async_result_t<void>` 계약(단일 consumer, 이동 전용, 이미 terminal이면 `await_ready()` true)은 그대로다 | **채택** (§4) |
| 2 | 즉시 admission 결과를 프로세스 공유 singleton `async_result_state_t<void>`로 반환해 할당을 0으로 | `take()`의 "already consumed" 판정이 인스턴스 상태다. singleton이면 두 번째 소비가 조용히 성공한다 — 공개 error 동작 변경 | 미채택: 공개 동작 보존 |
| 3 | 즉시 admission 결과 객체를 thread-local freelist로 재사용 | 늦은 completion이 참조할 수 없는 객체라 ABA는 없지만, 가이드 §4가 "public wrapper, Future/Task의 pool 재사용"을 명시적으로 기각한 범위다. 남는 이득도 send당 `operator new` 1회(≈2~3% Ir)뿐 | 미채택: 가이드 §4 기존 기각 |
| 4 | 2-part async send에서 borrowed native view(`zlink_msg_init` + `zlink_msg_copy` + `zlink_msg_close` × part)를 없애고 native part를 move | 그 복사본이 곧 거절 시 재전송용 원본 보존 수단이다. move하면 BACKPRESSURED 때 payload를 잃어 계약 (b)를 깬다. `zlink_msg_copy`는 64 B 초과 본문에서 refcount 공유라 65536B 비용도 작다 | 미채택: ownership 계약 위반 |
| 5 | C++ large-message buffer pool | 가이드 §4 확정 기각 | 미채택 |
| 6 | scheduler `std::function`을 함수 포인터로 | pass 2에서 이미 no-go(공개 header ABI) | 미채택: 기존 no-go |
| 7 | entry/Future/coroutine frame pool, wrapper pool | pass 2 no-go(ABA·ownership) | 미채택: 기존 no-go |
| 8 | reply가 이미 도착한 경우 suspend 생략 / 2-part staging inline화 / map node pool 확대 | pass 2에서 중복 또는 no-go | 미채택 |
| 9 | perf 러너의 `measurement_part_count()`가 메시지마다 `std::getenv`를 호출하는 것을 C처럼 static 캐시로 | **러너 변경이므로 이 pass의 개선으로 인정하지 않는다**(§5). 다만 측정된 비용이 커서 §8-1에 감독자 판단 항목으로 올린다 | 미채택: 범위 밖(러너) |
| 10 | in-flight 상한·timeout·client 수·sleep으로 수치 만들기 | §5 금지 | 미채택 |

### 3.1 후보 9의 근거 수치 (라이브러리 개선이 아니므로 참고용)

`bindings/cpp/perf/multi/common/perf_common.hpp:102`의 `measurement_part_count()`는 호출마다
`std::getenv("PERF_PART_COUNT")`를 부른다. C 기준 러너는 같은 값을 함수 지역 static으로 한 번만 읽고
그 이유까지 주석으로 적어 두었다(`bindings/c/perf/common/perf_zlink_part_helpers.hpp:13-24`,
"per-call getenv only adds hot-path noise to every send/recv").

64B before profile에서 `getenv` self 50,953,753 Ir + 그 안의 `__strncmp_avx2` 27,474,096 Ir =
78.4 M Ir = 전체의 **10.6%**, 메시지당 **1,253 Ir**. 호출 수도 C++ client 80,013회(send당 1회)
대 C client 201회(전체)로 갈린다. client뿐 아니라 server의 `measurement_parts_valid()`도
메시지당 2회 호출한다. **이 pass에서는 고치지 않았고**, before/after 양쪽에 똑같이 들어 있어
아래 개선 폭에는 포함되지 않는다.

---

## 4. 채택한 변경 (`bindings/cpp/**`, 공개 헤더 diff 0줄)

| 파일 | 변경 |
|------|------|
| `src/Runtime/Messaging/send_operations.cpp` | `submit_send_awaitable()`이 **entry·async state·waiter map 없이 첫 DONTWAIT admission을 먼저 시도**한다. 즉시 admission이면 source ownership만 detach하고 pooled operation state를 pool로 돌려준 뒤, 이미 terminal인 가벼운 결과를 돌려준다. Core가 record를 거절한 경우에만 그때 bundle(result + entry)을 만들고 owner에 등록한다. 제출은 되었는데 bundle 할당이 실패한 경로에서는 그 context로 park된 drain을 먼저 풀어 준다 |
| `src/Runtime/Messaging/async_operation_state.hpp` | 즉시 admission 전용 `immediate_send_result_t` 추가. `ready()`는 항상 true, `suspend()`는 false(이미 terminal), `take()`는 두 번째 소비에서 기존과 같은 `std::logic_error`를 던진다. mutex·condition variable·scheduler·continuation slot이 없다 |
| `src/Runtime/Messaging/completion_owner.hpp`·`.cpp` | `completion_entry_t`가 Core에 기록된 completion user context(`_context`)를 들고 다닌다. SEND는 entry가 소유하는 operation state 주소, REQUEST는 종전대로 entry 자신이다. `capture()`의 context 대조와 owner의 waiter map·`_early_send_completions` key·`unregister_entry()`가 모두 이 context를 쓴다. SEND 전용 `start_send()`/`submit_send_attempt(initial_, defer_source_detach_)`는 재시도 전용 `resubmit_send_attempt()`로 좁혔다(첫 제출이 terminal로 옮겨갔으므로 `initial_` 분기가 죽은 코드가 된다) |
| `src/Runtime/Messaging/request_reply.cpp` | `unregister_entry(entry.get())` → `unregister_entry(entry->context())`. REQUEST의 context는 여전히 entry 자신이라 동작은 같다 |
| `tests/contract/test_cpp_contract_optimization_guard.cpp` | guard를 "submit이 register보다 먼저"에서 **"첫 DONTWAIT 제출 → 즉시 admission 결과 → (거절 시에만) bundle 생성 → 등록" 순서**로 강화 |
| `tests/contract/test_cpp_contract_exact_target_retry.cpp` | 회귀 테스트 `test_admitted_async_send_leaves_no_waiter_identity()` 추가 |

### 4.1 SEND context를 operation state로 바꾼 것의 안전성

- 즉시 admission된 send에는 wait token이 없다(현행 코드도 `admitted && completion_id != 0`을 EPROTO로
  단정한다). 따라서 그 context 값을 참조할 completion record가 존재하지 않는다.
- 거절된 send의 operation state는 **entry가 `_send_operation`으로 소유**하며, entry가 죽을 때에야
  thread-local pool로 돌아간다. 즉 token이 살아 있는 동안 그 주소는 다른 operation에 재사용되지 않는다.
- 등록 전에 WRITABLE이 먼저 drain될 수 있는 창은 종전과 동일하다(제출 → 등록 순서가 그대로다).
  `_early_send_completions`가 그 창을 context key로 이어 준다.

### 4.2 추가한 회귀 테스트

`test_cpp_contract_exact_target_retry.cpp::test_admitted_async_send_leaves_no_waiter_identity()`
(실제 socket, inproc, 저 HWM):

1. 즉시 admission되는 async send 8건을 연속 실행 → 매번 part가 소비되고 `await_ready()`가 **바로**
   true여야 한다(= completion entry가 만들어지지 않았다는 관찰 가능한 증거).
   이 8건이 pooled operation state를 계속 재활용해 context 주소를 돌려쓰게 만든다.
2. 그 뒤 HWM까지 채워 async send 1건을 거절시킨다 → `await_ready()`가 false여야 한다.
3. 손으로 재시도하지 않고 Core의 WRITABLE만으로 terminal에 도달하는지, payload가 **정확히 1회**
   전달되는지 확인한다.

---

## 5. after 측정

같은 조건·같은 Core artifact·같은 C `p2cpp` 기준.

### 5.1 공식 after (`perf_cpp_multi_linux_20260907_112120_p2cpp_after.txt`, `status: complete`, `success: 5`, `actual_result_lines: 25`)

| Size | C `p2cpp` | C++ before | **C++ after** | 변화 | before/C | **after/C** |
|------|---:|---:|---:|---:|---:|---:|
| 64 | 1,682,102.0 | 1,151,636.0 | **1,316,504.2** | +14.32% | 68.46% | **78.27%** |
| 256 | 1,533,516.0 | 1,080,648.0 | **1,201,974.8** | +11.23% | 70.47% | **78.38%** |
| 1024 | 1,224,382.0 | 1,142,451.0 | **1,253,217.4** | +9.70% | 93.31% | **102.36%** |
| 4096 | 640,748.0 | 618,912.0 | **631,038.6** | +1.96% | 96.59% | **98.48%** |
| 65536 | 161,487.0 | 88,712.0 | **179,621.4** | **+102.48%** | 54.93% | **111.23%** |

**throughput aggregate 76.75% → 93.74%** (+16.99%p). 기본 목표 95%에 1.26%p 미달, 완화 목표 90% 충족.
개별 최소 85% 미달 셀: 64B 78.27%, 256B 78.38%.

| Size | C latency(ms) | C++ before(ms) | C++ after(ms) | after/C |
|------|---:|---:|---:|---:|
| 64 | 0.062 | 0.064 | 0.082 | 1.32x |
| 256 | 1.236 | 0.171 | 0.206 | 0.17x |
| 1024 | 0.700 | 0.199 | 0.394 | 0.56x |
| 4096 | 767.687 | 862.788 | 811.782 | 1.06x |
| 65536 | 6.790 | 14.011 | 8.010 | 1.18x |

**latency aggregate 0.93x → 0.86x** — 2.0x 이내 통과. 개별 셀도 전부 2.0x 이내다.
before의 65536B 2.06x outlier가 1.18x로 내려갔다. (latency 시간원 변경은 §0 참고.)

### 5.2 확인 run (`perf_cpp_multi_linux_20260907_112228_p2cpp_after_confirm.txt`, `status: complete`)

값을 고르기 위한 재측정이 아니라 65536B 이봉 거동(§2.5) 때문에 재현성을 남기려고 한 번 더 돌렸다.
**판정에는 §5.1의 공식 after를 쓴다.**

| Size | 확인 run | /C |
|------|---:|---:|
| 64 | 1,300,241 | 77.30% |
| 256 | 1,224,769 | 79.87% |
| 1024 | 1,234,973 | 100.86% |
| 4096 | 600,197 | 93.67% |
| 65536 | 182,052 | 112.74% |

aggregate 92.89%. 65536B의 개선(88.7k → 179.6k / 182.1k)은 두 run 모두에서 재현된다.

### 5.3 callgrind after (위치 확인, clients 4 / duration 3 / 64B)

| 항목 (64B) | before | after | 변화 |
|---|---:|---:|---:|
| 전체 Ir / 메시지 | 11,830 | **9,696** | **−18.0%** |
| `completion_entry_t` 생성 / send | 1.00 (79,823 / 79,823) | **0.0003 (25 / 88,614)** | 즉시 admission 경로에서 제거 |
| `register_send_entry` / send | 21건 | 25건 | 실제 backpressure만 |
| `operator new` / send | 1.028 | 1.026 | 결과 객체 1회(§3 후보 3) |
| callgrind 하 처리량 | 20,855 msg/s | 26,168 msg/s | +25.5% |

남은 send당 `operator new` 1회는 `immediate_send_result_t`의 `make_shared` 한 번이다. 이를 없애려면
후보 2(공개 동작 변경) 또는 후보 3(가이드 §4 기존 기각)이 필요해 채택하지 않았다.

---

## 6. 회귀 gate

### 6.1 테스트

| 항목 | 결과 |
|------|------|
| `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build bash bindings/cpp/tests/run_tests.sh` | **PASS** |
| contract | **19/19 PASS** (신규 회귀 테스트 포함) |
| sample smoke | **7/7 PASS** |
| `test_cpp_contract_request_reply`, `test_cpp_contract_request_writable_retry`, `test_cpp_perf_application_ready_queue`, `test_cpp_contract_optimization_guard`, `test_cpp_contract_writable_token_delivery`, `test_cpp_contract_completion_drain_order`, `test_cpp_contract_send_runtime_owner`, `test_cpp_contract_exact_target_retry` | 각 **5회 반복 PASS** |
| `test_cpp_send_close_stress` | **PASS** — `attempts=37803 accepted=27597 multipart_rejected=9915 backpressured=0 terminated=291 received=27597 ownership_failures=0 bad_records=0 close_ok=5 close_busy=0 unexpected=0` |
| 공개 API (`bindings/cpp/include/zlink`) | `git diff` **0줄** |
| Core | configure/build/clean 없음, artifact 동일 |

### 6.2 대상 외 대표 셀 (같은 조건에서 변경 전 build를 다시 만들어 paired 비교)

`--pattern MULTI_PUBSUB,MULTI_DEALER_ROUTER_REQREP --transports tcp --msg-sizes 64,1024 --duration 5 --runs 1`.
before 열은 내 5개 소스 파일만 원상복구해 다시 빌드한 build의 값이다(러너·Core·다른 변경은 그대로).

| 셀 | C | C++ before | C++ after | 처리량 변화 | after/C |
|---|---:|---:|---:|---:|---:|
| `MULTI_PUBSUB` 64B (Kmsg/s) | 2,344.142 | 1,916.826 | 1,894.549 | **−1.16%** | 80.82% |
| `MULTI_PUBSUB` 1024B (Kmsg/s) | 2,535.067 | 2,146.351 | 2,046.091 | **−4.67%** | 80.71% |
| `MULTI_DEALER_ROUTER_REQREP` 64B (Kops/s) | 397.123 | 262.706 | 262.089 | **−0.23%** | 65.99% |
| `MULTI_DEALER_ROUTER_REQREP` 1024B (Kops/s) | 353.791 | 263.284 | 263.963 | **+0.26%** | 74.61% |

| 셀 | C++ before 평균 latency(ms) | C++ after 평균 latency(ms) | 변화 |
|---|---:|---:|---:|
| `MULTI_PUBSUB` 64B | 1,388.575 | 1,318.179 | −5.07% |
| `MULTI_PUBSUB` 1024B | 960.001 | 1,011.962 | +5.41% |
| `MULTI_DEALER_ROUTER_REQREP` 64B | 11.076 | 11.055 | −0.19% |
| `MULTI_DEALER_ROUTER_REQREP` 1024B | 10.353 | 10.222 | −1.27% |

처리량 −5% 초과 하락 없음, 평균 latency +10% 초과 증가 없음 → **회귀 gate 통과**.
`MULTI_PUBSUB`은 동기 `publish()` terminal만 쓰므로 변경 경로에 닿지 않고, 두 REQREP 셀은
REQUEST context가 종전대로 entry 자신이라 동작이 같다는 코드 판정과 측정이 일치한다.

### 6.3 대상 pattern 내 다른 size

§5.1에 전부 있다. 하락한 셀 없음(+1.96% ~ +102.48%).

---

## 7. POSDDD 위험 신호 확인 (§7.7)

| 신호 | 전 | 후 |
|---|---|---|
| 단순 전달만 하는 helper·class 신설 | — | `immediate_send_result_t`는 전달자가 아니라 "이미 terminal인 결과"라는 상태를 직접 구현한다. 기존 `async_operation_state_t<void>`를 얇게 감싸지 않고 그 책임의 부분집합을 독립적으로 갖는다 |
| 죽은 분기 | `submit_send_attempt(initial_, defer_source_detach_)`의 `initial_==true` 분기와 `start_send()`가 첫 제출 전용이었다 | 첫 제출이 terminal로 옮겨가면서 두 파라미터가 모두 무의미해지므로 함수를 `resubmit_send_attempt()`로 좁히고 `start_send()`를 삭제했다(죽은 코드 잔존 0) |
| 같은 뜻이 두 곳에 | entry 주소가 "waiter map key"와 "Core user context" 두 역할을 겸했다 | `_context` 하나로 합쳐 SEND/REQUEST가 같은 규칙(“Core가 돌려줄 값이 key”)을 따른다 |
| 얕은 wrapper 추가 | — | 없음. 새 공개 타입·새 파일 없음 |
| 공개 계약 | — | 공개 헤더 diff 0줄, signature·enum·ownership·error 매핑 변경 없음 |

---

## 8. 감독자 판단이 필요한 항목

1. **C++ multi/single perf 러너의 hot path `getenv`** (§3.1). `measurement_part_count()`가 메시지마다
   `std::getenv`를 부른다. 64B에서 메시지당 1,253 Ir(전체의 10.6%)이고, C 기준 러너는 같은 값을
   static으로 캐시하며 그 이유를 주석으로 못 박아 두었다. 러너 정합 문제라 이 pass에서는 손대지
   않았다. 고칠지, 고친다면 어느 pass에서 할지 결정이 필요하다(고치면 C++ 수치가 더 오르지만
   그 상승분은 라이브러리 개선이 아니다). 대상: `bindings/cpp/perf/multi/common/perf_common.hpp:102`,
   같은 파일의 `measurement_parts_valid()`, `bindings/cpp/perf/single/**`의 동일 패턴.
2. **다른 작업이 이 pass 도중 C++ perf 러너 시간원을 바꿨다** (§0). after build에는 포함되고
   before report에는 없다. 처리량 비교는 무관하지만 latency 표의 before/after는 시간원이 다르다.
   이 상태로 판정을 확정할지, before를 다시 재고 latency를 갱신할지 결정이 필요하다.
3. **`MULTI_DEALER_DEALER` 판정.** aggregate 93.74%로 기본 목표 95%에는 1.26%p 미달, 완화 목표 90%는
   충족이다. 개별 최소 85% 미달은 64B(78.27%)·256B(78.38%). 완화 목표를 선택해 `통과`로 닫을지,
   64B/256B를 더 파고들지 결정이 필요하다. 남은 격차의 성격은 §5.3대로 send당 `operator new` 1회와
   §3.1의 러너 `getenv`이며, 전자는 가이드 §4가 이미 기각한 pool 없이는 줄일 수 없다.
4. **65536B 이봉 거동** (§2.5). after 두 run이 모두 높은 쪽으로 안정됐지만 원인을 끝까지 규명하지는
   못했다. 별도 확인이 필요하다고 보면 후속 pass 대상이다.
5. **커밋 범위.** 이 pass가 만든 변경은 `bindings/cpp/src/Runtime/Messaging/**` 5개 파일과
   `bindings/cpp/tests/contract/**` 2개 파일이다. 같은 작업 tree에 다른 작업의 미커밋 변경
   (`bindings/cpp/perf/**`, `bindings/dotnet/**`, `bindings/java/**`, `bindings/node/**`,
   `doc/plan/**`, `framework/**`)이 섞여 있으므로 커밋 시 경로를 골라야 한다.
