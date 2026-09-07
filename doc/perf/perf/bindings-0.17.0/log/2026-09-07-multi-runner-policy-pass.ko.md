# binding Multi 러너 정책 정합 pass 1 — 2026-09-07

> 범위: 설계 문서 `2026-09-07-runner-parity-design.ko.md` §3.2 B-4·B-6, §3.4 D-1·D-2,
> §5 단계 3·4 및 `2026-09-07-runner-conditions-pass1.ko.md` §4의 ③·⑤·⑥.
> `doc/perf/**` 정책 문서, 계획서, `decisions.ko.md`, `framework/**`, `core/**`,
> `bindings/c/perf/**`, binding 라이브러리 소스, single 러너는 **수정하지 않았다**.
> Core는 `core/build/lib/libzlink.so.0.17.0`(Build ID `af759a1c…`) 고정, 재빌드 없음.
> **perf 러너는 한 번도 실행하지 않았다.** 빌드·타입체크·contract test만 수행했다.
>
> **이 pass는 Core 0.17.1 rebase 때문에 중간에 종료했다.** 완료분과 잔여분을 §5·§6에
> 나누어 적었다. 다음 pass는 §6에서 시작한다.

---

## 0. 먼저 확정한 계약 (스펙·공개 헤더 인용)

| # | 질문 | 답 | 근거 |
|---|---|---|---|
| K1 | multi REQREP client 종료 순서 | client가 `RESULT`+`CLIENT_DONE` 출력 → socket 유지 → runner가 server `STOP`·종료 확인 → client `STOP` → close·exit | `PERF_POLICY.md:483-486`, `PERF_MULTI_TEST_POLICY.md:379,381,386-388` |
| K2 | multi REQREP server 종료 신호 | runner stdin `STOP`/`QUIT` (wire stop token 아님) | `PERF_POLICY.md:481`, `PERF_MULTI_TEST_POLICY.md:380,383`, C `perf_multi_socket_reqrep.hpp:1041-1053` |
| K3 | multi REQREP inflight 모델 | inflight를 인위적으로 고정하지 않고 **응답을 기다리지 않은 채** 연속 제출, reply completion은 동시 진행 | `PERF_MULTI_TEST_POLICY.md:164-168`, `:172-176`, `:186-189` |
| K4 | multi SENDSEND/one-way 모델 | socket당 미완료 async send terminal 1개, **admission 완료 즉시 다음 제출**, echo가 send를 gate하지 않음 | `PERF_MULTI_TEST_POLICY.md:137-148`, `:185-190`, `PERF_POLICY.md:162-165` |
| K5 | **async terminal의 backpressure 처리 주체** | **binding + Core.** 러너는 관측하지 않는다 | 아래 §0.1 |
| K6 | 패턴별 `CLIENT_READY`/`CLIENT_DONE` (C 기준) | DEALER_DEALER=READY+DONE, PUBSUB=READY+DONE, matched RR=READY+DONE, REQREP=**DONE만**, SENDSEND=**둘 다 없음** | `bindings/c/perf/multi/**` 전수 grep |
| K7 | request timeout knob | `PERF_MULTI_REQREP_TIMEOUT_MS`(기본 200) | C `perf_multi_socket_reqrep.hpp:573-574`, `PERF_POLICY.md:169-172` |
| K8 | 종료 보조 poll wait | C `perf_aux_poll_wait_ms()` = 100 ms | `bindings/c/perf/multi/common/perf_multi_poll.hpp:39` |

### 0.1 K5 — 7개 binding 공개 계약 전수 확인 (감독자 정정 지시의 "확인할 것")

**7개 모두 같은 계약이다.** DONTWAIT admission 1회 시도 → backpressure면 packet을 붙들고
**정확히 그 대기 토큰의 WRITABLE에서만** 재개 → admission 뒤 reply/terminal로 완료.
러너가 admission 경계를 관측할 필요가 없고, busy retry·timer·별도 worker thread를 쓰는
binding은 **없다**.

| binding | 공개 계약 위치 | 문구 |
|---|---|---|
| C++ | `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:320-324` | "Makes one DONTWAIT admission attempt. On backpressure, retains the request and resumes only from its exact WRITABLE token before retrying… The binding owns no retry timer or worker." |
| .NET | `bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs:165-178` | "Attempts non-blocking admission, waits for the exact WRITABLE token after backpressure, resubmits the same request… Core retains no request payload before admission" |
| Java | `bindings/java/src/main/java/systems/zlink/contracts/messaging/RequestOperation.java:10`, `SendSubmitOperation.java:26` | "…WRITABLE token allows a retry", "retries the retained packet only for the matching WRITABLE token" |
| Node | `bindings/node/src/zlink/contracts/messaging/operations.ts:25`, `:48-53` | "Backpressure waits for this request's WRITABLE token before resubmitting the same packet; the reply timeout starts only after admission." |
| Go | `bindings/go/contracts/sockets.go:38-45`, `:79` | "…a terminal that retries after its exact WRITABLE token before awaiting the reply result", "DONTWAIT back-pressure uses WRITABLE tokens instead" |
| Rust | `bindings/rust/src/contracts/messaging/operations.rs:143-150`, `:175-182` | "…the future retains the multipart request, waits for its exact WRITABLE token, and resubmits it once." |
| Python | `bindings/python/src/zlink/contracts/sockets/operations.py:29-36` | "Each attempt is nonblocking… `BACKPRESSURED`/`EAGAIN` waits for the matching WRITABLE token before resubmitting the same packet." |

**결론:** 러너는 `async()`를 await 없이 계속 부르고 완료되는 awaitable을 drain하기만 하면
된다. Core가 C 기준과 **같은 메커니즘**으로 이미 정확히 조절한다.
러너에 남는 유일한 상한은 **미완료 awaitable 객체의 메모리 상한**이다(§2.2).

> Go의 request terminal은 blocking `Submit(ctx) ([]*Message, error)` 하나뿐이다
> (`bindings/doc/spec/async-coroutine-policy.ko.md` §6). Go에서는 goroutine 하나가
> blocking terminal 하나를 소유하고 여러 goroutine을 동시에 진행시키는 형태가 같은 계약의
> Go 표현이며(`PERF_MULTI_TEST_POLICY.md:83-86`), 동시 goroutine 수가 곧 §2.2의 상한이 된다.

---

## 1. 작업 A — Multi 종료 protocol 정합 (설계 §3.4 D-1) — **완료**

| binding | 파일:줄 | 변경 | 근거 정책 |
|---|---|---|---|
| C++ | `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:39-56` | `wait_for_runner_stop_after_done()` 추가 (C `perf_multi_socket_reqrep.hpp:722-731` 준용) | `PERF_MULTI_TEST_POLICY.md:386-388` |
| C++ | 〃 `:287-294` | `RESULT` 뒤 `CLIENT_DONE,<size>` → stdin `STOP` 대기 → 그 뒤 socket close | `PERF_POLICY.md:483-486` |
| C++ | `bindings/cpp/perf/run_comparison.py:71-78` | `SOCKET_REQREP_PATTERNS` 상수 | C `run_comparison.py:66-69` |
| C++ | 〃 `:2379-2391` | `stop_client()` 추가 | `PERF_MULTI_TEST_POLICY.md:381` |
| C++ | 〃 `:2695-2703` | `CLIENT_DONE` 수신 시 REQREP은 `stop_server()` → `stop_client()` 순 | C `run_comparison.py:2653-2658` |
| .NET | `.../src/PerfMultiSocketReqRep.cs:406-419` | `CLIENT_DONE` 출력 + `WaitForRunnerStopAfterDone()` | `PERF_POLICY.md:483-486` |
| .NET | 〃 `:434-448` | `WaitForRunnerStopAfterDone()` 신규 | 〃 |
| .NET | `perf/multi/run_benchmarks.sh:575-587` | `pattern_uses_control_pipe`에 REQREP 2종 추가(server stdin STOP 경로 확보) | `PERF_MULTI_TEST_POLICY.md:380,383` |
| .NET | 〃 `:633-657` | `wait_for_client_done_line()` 신규 | 〃 |
| .NET | 〃 `:1963-2029` | REQREP branch: `CLIENT_DONE` 대기 → server `STOP`+종료 확인 → client `STOP` → client 종료 확인 | `PERF_MULTI_TEST_POLICY.md:379-381` |
| Java | `.../PerfMultiSocketReqRep.java:57-81` | server를 wire stop token 카운트 → **stdin STOP watcher + 100 ms aux wait** | `PERF_POLICY.md:481`, C `:1041-1053`, `perf_multi_poll.hpp:39` |
| Java | 〃 `:138-156` | client가 `RESULT` 직접 출력 → `emitClientDone` → `awaitStop` → `Result.silent` | `PERF_POLICY.md:483-486` |
| Java | 〃 `:255-259` | client의 **wire stop token 송신 제거** | `PERF_POLICY.md:481` |
| Java | `perf/multi/run_benchmarks.sh:1168-1200` | REQREP branch(동일 순서) | `PERF_MULTI_TEST_POLICY.md:379-381` |
| Node | `perf/multi/perf_multi_socket_reqrep.ts:120-133` | `CLIENT_DONE` 뒤 stdin `STOP` 대기(없으면 실패) | 〃 |
| Node | `perf/multi/perf_multi_orchestrator.ts:744-806` | REQREP 경로: `CLIENT_DONE` 대기 → `stopServer` → client `STOP` → exit 확인 | 〃 |
| Rust | `perf/multi/src/perf_multi_socket_reqrep.rs:333-360` | `CLIENT_DONE` 출력 + `wait_for_runner_stop_after_done()` 신규 | 〃 |
| Rust | `perf/run_benchmarks_multi.sh:1010-1060` | REQREP branch, 이후 공통 `shutdown_server` 중복 호출 방지 | 〃 |
| Python | `perf/multi/perf_multi_reqrep_client.py:38-49`, `:253-262` | `_wait_for_runner_stop_after_done()` + `CLIENT_DONE` 뒤 대기 | 〃 |
| Python | `perf/multi/run_benchmarks.py:1076-1141` | REQREP branch(동일 순서) | 〃 |
| Go | — | **이미 준수.** client `perf_multi_socket_reqrep.go:174-176`, runner `run_benchmarks_multi.sh:1320-1325`, `:1382-1397`. 대조만, 수정 없음 | 〃 |

### 1.1 REQREP server 종료 신호 전수 확인

C / C++ / .NET / Node / Go / Rust / Python = stdin `STOP`/`QUIT` (이미 준수).
**Java만 wire stop token ×clients** 였고 stdin watcher로 교체했다.

---

## 2. 작업 B — Multi 부하 수준 정합 (감독자 정정 지시 반영)

### 2.1 SENDSEND / multi one-way client — socket당 admission 모델

`PERF_MULTI_TEST_POLICY.md:68-69`가 send 계열에는 "admission 완료를 await한 뒤 다음 send
제출"을 명시적으로 허용한다.

| binding | before | after | 파일:줄 |
|---|---|---|---|
| C(기준) | socket당 retained message 1개 + `POLLOUT` 재제출 | — | `perf_multi_dealer_dealer_client.cpp:424-450` |
| C++ | socket당 coroutine `co_await …async()` | 변경 없음 ✓ | `perf_dealer_router_client.cpp:161-204` |
| .NET | slot당 `PendingAdmission` + `PerfMultiAdmissionSignal` | 변경 없음 ✓ | `PerfMultiDealerRouterClient.cs:193-240` |
| Java | socket당 admission round-robin | 변경 없음 ✓ | `PerfMultiRoutedSendCoordinator.java:24-66` |
| Node | socket당 `available[]` admission gate | 변경 없음 ✓ | `perf_multi_routed_sendsend.ts:96-160` |
| Go | socket당 goroutine × blocking `Send` (정책 `:83-86` 명시 허용) | 변경 없음 ✓ | `perf_multi_dealer_router.go:105-131` |
| Rust | slot당 `tasks.is_pending(slot)` gate | 변경 없음 ✓ | `perf_multi_dealer_router_client.rs:91-118` |
| Python | **`op.submit_sync()` 동기 terminal + socket 간 직렬화** | **socket당 async send coroutine이 admission을 await하고 즉시 다음 제출**, recv drain은 별도 coroutine, 종료는 bounded send drain | `perf_multi_common.py:367-401`(sync 경로 삭제), `perf_multi_dealer_router_client.py:74-146`, `perf_multi_router_router_client.py:76-148` |

Python이 어긴 조항: `PERF_MULTI_TEST_POLICY.md:92-93`, `:61-71`, `:137-148`.

### 2.2 REQREP — socket당 in-flight 모델 before/after

목표 모델(감독자 정정): **await하지 않고 `async()`를 연속 제출 → 완료되는 awaitable부터
drain → 미완료 개수 상한 하나만.**

| binding | before | after | 상태 |
|---|---|---|---|
| C(기준) | turn당 socket 1건 제출, reply gate 없음, Core admission backpressure에서만 그 socket을 건너뜀 | (수정 대상 아님) | — |
| **C++** | **reply 완료까지 socket당 1건 고정**(`operation_active` gate) | turn당 socket 1건 연속 제출, reply gate 없음, **socket당 미완료 상한 64** | **완료** |
| **Java** | **reply 완료까지 socket당 1건 고정**(`inFlight` gate) | 〃 | **완료** |
| .NET | turn당 socket 1건, 상한 없음 | 상한 미적용 | **잔여** |
| Node | turn당 socket 1건, 상한 없음 | 상한 미적용 | **잔여** |
| Rust | turn당 socket 1건, 상한 없음 | 상한 미적용 | **잔여** |
| Python | turn당 socket 1건, 상한 없음 | 상한 미적용 | **잔여** |
| Go | **socket당 goroutine 1개 × blocking `Submit` = in-flight 1** | 미착수 | **잔여(위반 상태)** |

변경 파일:줄 —
C++ `common/perf_multi_reqrep.hpp:44-56`(`max_outstanding_per_socket()`),
`:113-128`(slot 카운터), `:305-311`(cap gate), `:435-441`·`:462`·`:481`(카운터 증감),
`:525-527`(멤버).
Java `PerfMultiSocketReqRep.java:180-184`(카운터 배열), `:203-206`(cap gate),
`:275-297`(`resolveMaxOutstandingPerSocket()`), `:340-350`(증감).

### 2.3 상한 값과 근거

- **knob:** `PERF_MULTI_REQREP_MAX_OUTSTANDING`, **requester socket 1개당** 미완료 request
  awaitable 수. **기본값 64**, 7개 binding 공통.
- **근거:**
  1. 이 상한은 **wire 조절 장치가 아니다.** §0.1대로 admission 조절은 이미 Core와 binding이
     C와 같은 메커니즘으로 수행한다. 러너 상한의 유일한 목적은 "wire가 HWM으로 막혀 있는
     동안 미완료 awaitable 객체만 메모리에 쌓이는 것"을 막는 것이다.
  2. 따라서 값은 **정상 상태 depth보다 충분히 커야** 한다. Little's law로
     `depth ≈ (socket당 완료율) × RTT`. 기본 조건(clients=100, tcp, loopback)에서
     socket당 완료율은 전체 throughput/100이고 RTT는 ms 단위이므로 정상 상태 depth는
     **한 자릿수**다. 64는 그보다 한 자릿수 이상 크다.
  3. 상한에 닿는 것은 곧 `PERF_MULTI_REQREP_TIMEOUT_MS`(200 ms) 초과가 발생하는 영역이며,
     그 자체가 실패 조건이다.
  4. in-flight 1은 금지(`PERF_MULTI_TEST_POLICY.md:164-168`)이므로 하한을 2로 clamp했다.
- **검증 방법(감독자용):** 같은 조건을 `PERF_MULTI_REQREP_MAX_OUTSTANDING=256`으로 한 번 더
  돌려 RESULT가 바뀌지 않으면 상한이 측정을 제한하지 않는다는 증거가 된다(§7.2).
- **Effective Options 노출은 아직 하지 않았다(§6-2).** C 러너에는 대응 행이 없으므로
  노출하면 7개 binding 전부가 C와 1행 달라진다. 감독자 판단이 필요하다(§8-1).

### 2.4 PUB/XPUB publish와 raw reply — 7개 binding 재확인 (변경 없음)

전부 synchronous terminal 유지.
publish: cpp `perf_pubsub_server.cpp:95` / dotnet `PerfMultiPubSubServer.cs:63-66` /
java `PerfMultiPubSub.java:186-189` / node `perf_multi_runtime.ts:277-288` /
go `perf_multi_pubsub.go:239` / rust `perf_multi_pubsub_server.rs:69-74` /
python `perf_multi_common.py:398-406`.
raw reply: cpp `perf_multi_reqrep.hpp:527` / dotnet `PerfMultiSocketReqRep.cs:412` /
java `PerfMultiSocketReqRep.java:85` / node `perf_multi_runtime.ts:41-43` /
go `perf_multi_socket_reqrep.go:90` / rust `perf_multi_socket_reqrep.rs:171` /
python `perf_multi_reqrep_server.py:19-30`.

---

## 3. 작업 C — 그 밖의 실행 모델·형식 정합 (완료분)

### 3.1 D-2 — 패턴별 token을 C 기준으로 통일

| binding | before | after |
|---|---|---|
| Node SENDSEND | `CLIENT_READY` + runner `START` 대기(client·server) + `CLIENT_DONE` | **전부 제거** (C SENDSEND는 어느 token도 쓰지 않는다) |
| Node REQREP | `CLIENT_READY` + `START` 대기 | **제거**, `CLIENT_DONE`만 유지 |
| Node runner | `needsClientReady`/`needsRunnerStart`가 9개 패턴 전부 | DEALER_DEALER·PUBSUB·STREAM 3개로 축소 |
| Rust PUBSUB | `CLIENT_DONE` 없음 | 추가 |
| Python PUBSUB | `CLIENT_DONE` 없음 | 추가 |
| C++·.NET·Java·Rust·Python REQREP | `CLIENT_DONE` 없음 | 추가(§1) |

근거 `PERF_POLICY.md:469-471`, `:487-496`.
파일: `perf_multi_routed_sendsend.ts:211-216`·`:288-291`·`:323-333`,
`perf_multi_socket_reqrep.ts:53-58`, `perf_multi_orchestrator.ts:456-476`,
`perf_multi_pubsub_client.rs:161-163`, `perf_multi_pubsub_client.py:143-147`.

### 3.2 timeout knob 이름 — C++ REQREP

`common/perf_multi_reqrep.hpp:39-42`, `:322-326` — request timeout을
`PERF_MULTI_RCVTIMEO_MS`(소켓 recv timeout)에서 `PERF_MULTI_REQREP_TIMEOUT_MS`(기본 200)로
교체. 근거 `PERF_POLICY.md:169-172`, C `perf_multi_socket_reqrep.hpp:573-574`.
두 기본값이 모두 200이라 **현재 측정 조건은 바뀌지 않는다.** 나머지 6개는 이미 정합.

### 3.3 조건 정합 log §4 이월 항목

| # | 항목 | 처리 |
|---|---|---|
| ⑤ | Java `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 부재 | **완료.** `PerfMultiRoutedSendCoordinator.java:22-44` `sendDrainTimeout()`(기본 5000) 추가, `PerfMultiDealerDealer.java:188`·`PerfMultiDealerRouter.java:134`·`PerfMultiRouterRouter.java:139`의 임시 상수 `duration+5s` 대체 (`PERF_MULTI_TEST_POLICY.md` §12.3) |
| ⑤ | Python 부재 | **완료.** `perf_multi_common.py:279-287` `resolve_multi_send_drain_timeout_ms()`(기본 5000), SENDSEND 2종의 bounded drain에 적용 |
| ⑤ | Go 부재 | **잔여**(§6-3) |
| ⑥ | Python Effective Options `clients` 표기 | **완료.** `run_benchmarks.py:461-467` — `100 (stream=100)` 주석 제거, C `resolve_clients_meta`(`run_comparison.py:3830-3844`) 규칙과 동일 |
| ③ | Go/Rust/Python multi `AUTO_HWM_DETAIL` 부재 | **잔여**(§6-4) |

---

## 4. 검증 (perf 미실행)

| 대상 | 결과 |
|---|---|
| C++ `cmake --build … cpp_comp_src_{dealer_router,router_router}_reqrep_client` | 통과 |
| .NET `dotnet build -c Release` (Zlink.BindingBench.Multi) | 통과 (0 warning / 0 error) |
| Java `./gradlew :perf-multi:compileJava --offline` | 통과 |
| Rust `cargo build` (perf/multi 전체 바이너리) | 통과 |
| Node `tsc -p tsconfig.tools.json` + `dist-tools` 재생성 | 통과 |
| Node perf contract test 4종 | 통과 (10/1/5/4 = 20건) |
| Python `compileall bindings/python/perf/multi` | 통과 |
| `bash -n` java/dotnet/rust multi runner, `ast.parse` cpp/python runner | 통과 |
| Python `tests/test_perf_multi_runner.py` | **미실행** — 이 호스트 `python3`에 pytest 없음 |
| Java `:perf-multi:test` | **미실행** — `ZLINK_CORE_PACKAGE_PREFIX` 필요 |

---

## 5. 완료 요약

- 작업 A: **7개 binding 전부 완료**(Go는 이미 준수, 대조만).
- 작업 B SENDSEND: **완료**(Python 1건 수정, 나머지 6개는 이미 정합임을 확인).
- 작업 B REQREP: **C++·Java 완료**(reply gate 제거 + 상한 64). **.NET·Node·Rust·Python은
  상한만 미적용**, **Go는 미착수**.
- 작업 C: D-2 token 정합, C++ timeout knob, 조건 log ⑤(Java·Python)·⑥ 완료.

모든 변경 파일은 컴파일·문법이 통과하는 일관된 상태다. 반쯤 고친 파일은 없다.

---

## 6. 다음 pass가 이어갈 잔여 항목

1. **REQREP 미완료 상한 적용 — .NET·Node·Rust·Python.**
   각 러너는 이미 "turn당 socket 1건, reply gate 없음"이므로 **socket당 미완료 카운터 +
   `PERF_MULTI_REQREP_MAX_OUTSTANDING`(기본 64) gate만 추가**하면 된다. 착수 지점:
   - .NET `PerfMultiSocketReqRep.cs:322-352`(`ClientSlot`에 카운터, `RunClientLoopAsync`의
     submit 루프에 gate, `ObserveRequestAsync` finally에서 감소)
   - Node `perf_multi_socket_reqrep.ts:96-116`(socket별 카운터 배열, `submitRequest`
     finally에서 감소)
   - Rust `perf_multi_socket_reqrep.rs:267-302`(`ConcurrentTasks`가 slot 키를 쓰지 않고
     `new(0)`으로 push하므로 socket별 카운터를 별도로 들고 `process_completion` 경로에서
     감소시켜야 한다 — 여기만 구조 확인이 필요하다)
   - Python `perf_multi_reqrep_client.py:171-200`(socket별 카운터, `observe_done`에서 감소)
2. **Go REQREP 재구성 (§2.2의 유일한 정책 위반 잔여).**
   `bindings/go/perf/multi/perf_multi_socket_reqrep.go:178-237`.
   설계: socket당 driver goroutine이 크기 `PERF_MULTI_REQREP_MAX_OUTSTANDING`의 버퍼 채널을
   세마포어로 쓰고, 토큰을 얻을 때마다 request goroutine 하나를 띄운다(각 goroutine이
   blocking `Submit(ctx)` 하나 소유 = `PERF_MULTI_TEST_POLICY.md:83-86`의 Go 표현).
   `countsByClient[i]`가 socket당 여러 goroutine에서 갱신되므로 `atomic.AddUint64`로 바꿔야
   한다. `perfcommon.Stats`가 이미 goroutine 간 동시 호출을 받고 있으므로 stats는 그대로.
3. **Go send drain(조건 log ⑤).** Go SENDSEND client는 active deadline에서 **소켓을 닫아**
   대기 중 admission을 깨우고(`perf_multi_dealer_router.go:160-168`), DEALER_DEALER·
   ROUTER_ROUTER는 **무한 `WaitGroup.Wait()`**(`perf_multi_dealer_dealer.go:250`,
   `perf_multi_router_router.go:159`)이다. bounded drain을 넣으려면 drain 구간 동안 각
   패턴의 reply drain을 살려 두어야 해서 3개 client teardown을 재구성해야 한다.
   `PERF_POLICY.md:544-549`가 이 drain을 "수행할 수 있다"로 규정하므로 위반은 아니다.
4. **조건 log ③ — Go/Rust/Python multi `AUTO_HWM_DETAIL`.**
   Go는 `perfcommon.PrintSocketAutoHWMDetail`이 있으나 **필드 집합이 C와 다르고**(C는
   `label/source/enabled/role_id/profile/policy_class/last_recalc_*/deferred_*`까지),
   Rust·Python은 emitter 자체가 없다. 더 중요한 것은 **채취 지점**인데 조건 정합 pass §5-4가
   relay/pubsub server의 채취 지점을 감독자 판단 항목으로 남겨 두었다. 지점을 정하지 않은
   채 emitter만 추가하면 조건 pass가 방금 없앤 `4096000` 오보고를 3개 러너에 새로 만든다.
   **§5-4 결정 뒤 별도 pass**를 권한다.
5. **Effective Options에 `reqrep_max_outstanding` 노출**(§8-1 결정 뒤).
   7개 러너의 Effective Options 생성부 + `bindings/python/perf/perf_report.py`(Rust·공용
   `render-multi` 경로)에 각각 1행씩 추가하면 된다.
6. **[신규 발견] multi metric header 시간원 통일.** §8-4 참조. 이번 범위 밖.
7. **[신규 발견] .NET SENDSEND server가 stdin `STOP`을 받지 못한다.** §8-5 참조.

---

## 7. 감독자가 실행해야 할 smoke 목록

모두 **직렬**, `PERF_FAIL_FAST=1`, `--duration 1 --runs 1 --msg-sizes 64 --transports tcp`,
`load_avg < 10`. 측정 조건(duration/HWM/timeout/client 수)은 하나도 완화하지 않았다.
Core artifact가 0.17.1로 바뀌면 C 기준값도 같은 세션에서 새로 짝지어야 한다(계획서 §7.0).

### 7.1 필수 — 이번 변경이 직접 건드린 경로

| # | 러너 | pattern | 확인 항목 |
|---|---|---|---|
| 1 | C++ | `MULTI_DEALER_ROUTER_REQREP` | stdout에 `CLIENT_DONE,64`, client가 **server 종료 뒤** exit 0, drain timeout 로그 없음, `status: complete` |
| 2 | C++ | `MULTI_ROUTER_ROUTER_REQREP` | 〃 |
| 3 | .NET | `MULTI_DEALER_ROUTER_REQREP` | 〃 + server가 stdin STOP으로 exit 0 |
| 4 | Java | `MULTI_DEALER_ROUTER_REQREP` | 〃 + server가 **wire stop token 없이** stdin STOP으로 종료, client RESULT 5줄이 정확히 1번만 출력 |
| 5 | Java | `MULTI_ROUTER_ROUTER_REQREP` | 〃 |
| 6 | Node | `MULTI_DEALER_ROUTER_REQREP` | 〃 + `CLIENT_READY`/`START`가 더 이상 오가지 않는데도 정상 종료 |
| 7 | Rust | `MULTI_DEALER_ROUTER_REQREP` | 〃 |
| 8 | Python | `MULTI_DEALER_ROUTER_REQREP` | 〃 (`ZLINK_LIBRARY_PATH=<core>/lib/libzlink.so.<ver>` 필요) |
| 9 | Go | `MULTI_DEALER_ROUTER_REQREP` | 코드 무변경 — 회귀 없음 확인 |
| 10 | Node | `MULTI_DEALER_ROUTER_SENDSEND` | barrier 제거 뒤에도 `status: complete`, server가 STOP으로 종료 |
| 11 | Node | `MULTI_ROUTER_ROUTER_SENDSEND` | 〃 |
| 12 | Python | `MULTI_DEALER_ROUTER_SENDSEND` | `submit_sync` → async 전환 뒤 `status: complete`, throughput before/after 기록, send drain timeout 로그 없음 |
| 13 | Python | `MULTI_ROUTER_ROUTER_SENDSEND` | 〃 |
| 14 | Rust | `MULTI_PUBSUB` | `CLIENT_DONE` 추가가 runner 파싱을 깨지 않는지 |
| 15 | Python | `MULTI_PUBSUB` | 〃 |
| 16 | Java | `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_SENDSEND` | send drain knob 교체 뒤 회귀 없음 |
| 17 | Python | 전체 report 1회 | Effective Options `clients` 행이 C와 일치 |

### 7.2 상한이 측정을 제한하지 않는지 (C++·Java REQREP)

같은 조건을 `PERF_MULTI_REQREP_MAX_OUTSTANDING=256`으로 한 번 더 돌린다.
RESULT가 기본값(64)과 유의미하게 다르면 상한이 측정을 제한하고 있다는 뜻이므로 값을
올리고 §2.3의 근거를 갱신해야 한다.

### 7.3 회귀 감시

REQREP 4종(C++·Java)은 **inflight 모델이 바뀌었으므로 수치가 달라진다.**
`PERF_MULTI_REQREP_TIMEOUT_MS` 초과가 0인지, 메모리 사용이 duration에 비례해 증가하지
않는지 함께 본다.

---

## 8. 감독자 판단이 필요한 사항

1. **`reqrep_max_outstanding`의 Effective Options 노출.** C 러너에는 대응 행이 없다.
   노출하면 7개 binding 전부가 C와 1행 달라져 조건 정합 pass의 "`lang` 외 완전 일치"가
   깨진다. 선택지: (a) C 러너에도 같은 행을 추가(=C 담당 에이전트 작업), (b) binding 전용
   행으로 정책 문서에 명시, (c) 노출하지 않고 로그에만 기록. 결정 전까지 노출하지 않았다.
2. **잔여 5개 binding의 상한 적용 시점.** §6-1·§6-2. Core 0.17.1 rebase 뒤 이어서.
3. **Go send drain(§6-3)을 이번 캠페인에서 다룰지.** 정책상 위반은 아니다.
4. **[신규 발견] multi metric header의 시간원이 binding마다 다르다.** 설계 문서 C-8은
   Go·Rust만 지목했으나 전수 확인 결과 **C만 monotonic**이다.
   - C `perf_multi_metric_header.hpp:40-47` — `steady_clock` ✅
   - C++ `perf/multi/common/perf_metric_header.hpp:64-69` — **`system_clock`(wall)**
   - Python `perf/perf_metrics.py:127` — **`time.time_ns()`(wall)**
   - .NET `PerfShared.cs:25-45` — `Stopwatch`를 `DateTime.UtcNow` epoch에 고정(**wall 기준점**)
   - Go·Rust — 설계 문서 C-8이 이미 지목
   `PERF_POLICY.md:127-141` 위반이다. C↔binding paired 비교는 각 pair가 자기 시간원으로
   닫히므로 값 자체는 성립하지만 D-095(이 호스트 wall clock ±5 s 점프) 위험에 노출된다.
   개정 문서 R13의 대상 목록이 Go·Rust로만 적혀 있으므로 **범위 확대 재배정**이 필요하다.
5. **[신규 발견] .NET SENDSEND server가 stdin `STOP`을 받지 못한다.**
   `run_benchmarks.sh`의 `pattern_uses_control_pipe`에 SENDSEND가 없어 server가 SIGTERM으로
   끝난다. C relay server는 stdin watcher로 graceful shutdown한다
   (`perf_multi_relay_server.hpp:667-677`). 설계 문서 D-1 목록에 없던 항목이라 넣지 않았다.
6. **Python `pytest` 부재.** 이 호스트 `python3`에 pytest가 없어
   `bindings/python/tests/test_perf_multi_runner.py`를 돌리지 못했다.

---

## 9. 변경 파일

```
bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp
bindings/cpp/perf/run_comparison.py
bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiSocketReqRep.cs
bindings/dotnet/perf/multi/run_benchmarks.sh
bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiSocketReqRep.java
bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiRoutedSendCoordinator.java
bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiDealerDealer.java
bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiDealerRouter.java
bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiRouterRouter.java
bindings/java/perf/multi/run_benchmarks.sh
bindings/node/perf/multi/perf_multi_socket_reqrep.ts
bindings/node/perf/multi/perf_multi_routed_sendsend.ts
bindings/node/perf/multi/perf_multi_orchestrator.ts
bindings/node/dist-tools/perf/multi/*.js            (위 TS의 추적 산출물)
bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs
bindings/rust/perf/multi/src/perf_multi_pubsub_client.rs
bindings/rust/perf/run_benchmarks_multi.sh
bindings/python/perf/multi/perf_multi_common.py
bindings/python/perf/multi/perf_multi_reqrep_client.py
bindings/python/perf/multi/perf_multi_dealer_router_client.py
bindings/python/perf/multi/perf_multi_router_router_client.py
bindings/python/perf/multi/perf_multi_pubsub_client.py
bindings/python/perf/multi/run_benchmarks.py
doc/perf/perf/bindings-0.17.0/log/2026-09-07-multi-runner-policy-pass.ko.md   (본 문서)
```

Go 러너(`bindings/go/perf/**`)와 C 러너(`bindings/c/perf/**`)는 수정하지 않았다.
