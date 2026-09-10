# 바인딩 submit 종결자 결과 enum — 적용 plan

> 설계: [`../draft/bindings-submit-result-terminal.ko.md`](../draft/bindings-submit-result-terminal.ko.md).
> 사용자 결정(2026-09-10): 호환성 없이 변경, 7언어 동시, perf는 `BACKPRESSURED`일 때만 비동기 대기.
> 릴리스: **bindings 0.18.0에 들어간다**(사용자 결정 2026-09-10). 순서는 Core `core/v0.18.0` 태그(Core는 이 캠페인과 무관) → 이 캠페인(#88~#101) → binding 4언어 `<lang>/v0.18.0` 태그 → framework 릴리스. Milestone `0.18.0`.
> 작업 방식: `doc/principal/dev/development-workflow.ko.md`. 스펙·정책·문서는 감독자, 코드는 codex job, 측정은 `perf-ticket.sh`.

## 0. 범위·전제

- 대상: `bindings/{cpp,dotnet,go,java,node,python,rust}` 공개 send·request 비동기 종결자, 그 내부 completion owner,
  contract test, perf multi·single 클라이언트, gRPC 벤치 raw 드라이버 4언어, framework 4언어의 binding 호출부, 스펙·정책·guide.
- 비대상: C API·Core(변경 없음), `submit_sync()`, publish, reply, STREAM.
- 기준 트리: PR #103(0.18.0 버전 범프) 머지 커밋. binding 버전은 이미 0.18.0이며 이 캠페인이 그 릴리스 내용이다.

## 1. 반영 순서 (단계 게이트)

| 단계 | 내용 | 담당 | 게이트 |
|---|---|---|---|
| G0 | #86 머지. 이름 확정: `SendSubmission`/`RequestSubmission`, `result`/`admitted`/`reply`; Go 비동기 종결자 이름; .NET `TrySend` 유지 여부 | 사용자·감독자 | 이 문서 §2 결정표 채움 |
| G1 (#88) | 스펙·정책 문안(§3) 커밋 | 감독자 | 표·문장 diff 검토 |
| G2 (#89) | **Java 파일럿**: 종결자·CompletionOwner·contract test·Kotlin 확장 | job `java-submit-result` | contract 통과, 성공 기준 1 |
| G3 (#90) | Java perf multi/single reqrep·sendsend 클라이언트를 §5 규칙으로; gRPC 벤치 Java raw 드라이버 | job `java-perf-submit-result` | 성공 기준 2·3 |
| G4 (#91 cpp · #92 dotnet · #93 node · #94 go · #95 rust · #96 python) | 나머지 6언어 병렬: 종결자·owner·contract test·perf 클라이언트 | job 6개(동시 ≤5, `env-job-concurrency-cap`) | 언어별 contract·perf 통과 |
| G5 (#97 java · #98 dotnet · #99 node · #100 cpp) | framework 4언어: 내부 binding 소비를 결과 객체로(F1), 동기 blocking 종결자 추가(F2·F2-a), samples·guide 코드 블록 | job 4개 | framework 테스트·e2e(cross-language)·F2-a 회귀 |
| G6 (#101) | 전체 perf 재측정(multi 전 패턴, 4 size, tcp) 전후 비교; gRPC 벤치 Java 3-run | 감독자 ticket | 성공 기준 4 |
| G7 (#101) | 릴리스 노트(breaking), 문서 검사(`check_doc_links`, `check_prose_neutrality`) | 감독자 | 완료 기준 |

G2 결과가 설계를 바꾸면 G4 전에 draft를 갱신한다. G4 언어별 job은 G2 커밋을 참조 구현으로 받는다.

## 2. 먼저 정할 것 (G0 결정표)

| # | 항목 | 결정 (2026-09-10, 사용자) |
|---|---|---|
| 1 | 결과 객체 이름 | `SendSubmission`/`RequestSubmission` (draft §4) |
| 2 | 필드 이름 | `result`, `admitted`, `reply` (언어 관례 대소문자) |
| 3 | 즉시 실패의 전달 | 예외/에러 유지(draft §3.1) |
| 4 | 종결자 이름 | **`bindings/doc/spec/async-coroutine-policy.ko.md` §6 표 그대로.** 새 이름을 만들지 않는다. 비동기 종결자(Java·Node·Python·Rust `submit()`, .NET `Async()`, C++ `async()`)의 반환형만 결과 객체로 바꾼다. 동기 종결자(`submit_sync()`, .NET·C++ `submit()`) 불변 |
| 5 | Go | **결정(2026-09-10)**: 정책 §6 이름 `Submit(context.Context)` 하나 유지. `Submit(ctx)`는 native 제출 한 번을 하고 **즉시** 결과 객체를 돌려주며, 대기는 결과 객체의 메서드가 한다 — `Result() SubmitResult`, `Admitted(ctx) error`(OK면 즉시 nil), request는 `Reply(ctx) ([]*Message, error)`. 채널 필드 대안(B-1)은 ctx 취소·채널 수명 부담으로 기각. 기존 호출부는 `parts, err := op.Submit(ctx)` → `sub, err := op.Submit(ctx); parts, err := sub.Reply(ctx)`. Go 스펙 `README.ko.md:132, :265`와 정책 §6 Go 행 수정. publish·reply 불변 |
| 6 | .NET `TrySubmit()` (send·reply, `OperationContracts.cs:54`) | **제거.** 결과 enum이 대체한다. 호출부 2곳 이관: `framework/languages/dotnet/.../ZLinkBackendStreamSocketWrapper.cs:356`, `ZLinkManagedMeshNode.cs:12447` |
| 7 | Rust `admitted`/`reply` 타입 | `Pin<Box<dyn Future<Output = …> + Send>>`로 시작. perf에서 회귀가 보이면 명명 타입으로 바꾼다 |
| 8 | Python `result` | 결과 객체 속성. publish는 불변 |
| 9 | 원칙 | 가능한 한 7언어가 같은 모양(객체·필드·대기 규칙)을 가진다 |

## 3. 스펙·정책 수정 대상 (G1, ko+en)

| 파일 | 절 | 수정 |
|---|---|---|
| `bindings/doc/spec/async-coroutine-policy.ko.md`·`.en.md` | §6 terminal interface 표 | 비동기 종결자 반환형을 결과 객체로; Go 행; §7 검증 요구에 `result`·`admitted` 항목 |
| `bindings/doc/spec/README.ko.md`·`.en.md` | `#submit-result-projection` 표 | `BACKPRESSURED` 행: 보관·WRITABLE 대기 유지 + "종결자는 `BACKPRESSURED`와 admission stage를 돌려준다"; `OK` 행: "`OK`와 완료된 admission stage" |
| 같은 파일 | 도메인 모델 `SubmitResult` 항목 | "exception 언어는 예외 `.code`로만" → "비동기 종결자의 결과 객체 `result`로도 노출" |
| 같은 파일 | send·request terminal 절, 언어별 § | 결과 객체 시그니처(draft §4) |
| `bindings/doc/spec/async-execution-model.ko.md`·`.en.md` | §5 | `result` 스냅샷·`admitted`·`reply` 합류와 정확히 한 번 규칙 |
| `bindings/doc/spec/<lang>/README.*` (7언어) | terminal 절 | 언어별 시그니처·예 |
| `doc/perf/PERF_MULTI_TEST_POLICY.md` | §1.1 "C 이외의 binding", §1.2 request/reply 클라이언트 | draft §5 규칙; "turn당 1건" 서술 제거 |
| `framework/bench/grpc/README.{ko,en}.md` | §2 request-backpressure, §10.3 | "`result == BACKPRESSURED`에서 멈추고 `admitted`에서 재개" 한 문장 |
| `bindings/doc/guide/**`, `core/doc/guide/**` 코드 블록 | send/request 예 | `.reply()`/`admitted` 사용 예 |

## 4. 바인딩 코드 수정 대상 (G2·G4)

| 언어 | 공개 | 내부 | 테스트 |
|---|---|---|---|
| java | `contracts/messaging/{Send,Request}SubmitOperation.java`, 새 `{Send,Request}Submission.java`, Kotlin 확장 | `runtime/sockets/CompletionOwner.java` `submitSend/submitRequest/retrySend/retryRequest`, `Pending`에 admitted future | `contract/*Completion*`, 새 `SubmitResultTerminalContractTest` |
| dotnet | `Contracts/Messaging/OperationContracts.cs` `Async()`; `TrySubmit()` 제거(send·reply) | `Runtime/Messaging/CompletionOwner.cs` entry `Arm*`/`Retry`, `TrySend` 경로 삭제 | contract |
| node | `contracts/messaging/operations.ts` | `runtime/messaging/completion_owner.ts` | contract |
| cpp | `include/zlink/Contracts/Messaging/operation_contracts.hpp` `async()` | `src/Runtime/Messaging/completion_owner.cpp`, `send_operations.cpp` | `tests/contract` |
| go | `internal/native/operations.go` `sendBuilder.Submit`/`requestBuilder.Submit` → `(SendSubmission, error)`/`(RequestSubmission, error)`, `root_projection.go` 노출 | writable retry 경로가 `Admitted(ctx)`를 완성; `Reply(ctx)`는 기존 reply 대기 | `writable_retry_test.go` 확장, surface_test 갱신 |
| rust | `src/contracts/messaging/operations.rs` | `runtime/messaging/operations/{send_ops,routed_async}.rs` | `tests/contract_tests.rs` |
| python | `contracts/sockets/operations.py` | `_runtime/sockets/socket_base_impl.py` bridge | tests |

## 5. perf·벤치 수정 대상 (G3·G4)

| 위치 | 변경 |
|---|---|
| `bindings/java/perf/multi/.../PerfMultiSocketReqRep.java` | poll pacing 제거. `result==BACKPRESSURED`면 `admitted` 뒤 재개; reply는 `reply` stage로 진행 |
| `bindings/java/perf/multi/.../PerfMultiRoutedSendCoordinator.java`, `PerfMultiAsyncSendLoop.java` | stage `isDone` 판정 → `result` 판정 |
| `bindings/{node,dotnet,cpp,go,rust,python}/perf/multi/*reqrep*`, `*sendsend*`, single | 같은 규칙 |
| `framework/bench/grpc/{java,node,dotnet,cpp}` raw 드라이버 | request-backpressure·send-saturation을 같은 규칙으로. Java는 Issue #12 브랜치(`e1272851bd` perf 구조) 위에서 |

## 6. framework 호출부 (G5)

| 언어 | 위치(예) | 변경 |
|---|---|---|
| java | `ZLinkJavaRawServicePort.java:211-219` 외 `router.request(...)`/`send(...)` 호출부 | `.submit()` → `.submit().reply()` / `.admitted()` |
| dotnet | `ZLinkBackendStreamSocketWrapper.cs:356`, `ZLinkManagedMeshNode.cs:12447` (`TrySubmit()` 사용) + raw mesh node/service port | `TrySubmit()` → `Async()` 결과의 `Result`/`Admitted`; 나머지 `.Reply` |
| node, cpp | 같은 역할의 raw mesh node/service port | 동일 |

framework의 hot path(08장 E1~E5)는 `.reply()`만 쓰므로 hop이 늘지 않는다. 결과 객체 할당 1회는 E2 측정에 포함해 확인한다.

### 6.1 framework 스펙 검토와 결정 (`framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md`, 2026-09-10, 사용자 결정)

framework는 binding 정책을 따르되, 다음 두 결정으로 범위를 정한다.

**결정 F1 — framework 메시징 공개 terminal은 backpressure를 노출하지 않는다.** 내부 socket을 숨기는 layer가 admission 상태를
돌려주면 호출자가 socket 수준 제어를 떠안는다. 공개 계약(§2 terminator 완료 의미, §4 one-way admission 경계, §5 "`Backpressured`는
public terminal result가 아니다", 04 §8 3단계 backpressure)은 그대로다. **framework 내부 구현**이 binding 결과 객체의
`result`·`admitted`를 소비해 "HWM에 걸렸을 때만 기다린다"를 정확히 구현한다. 지금까지는 stage 하나로 admission과 reply를 구분할
수 없어 3단계 대기가 정확하지 않았다. gRPC 벤치 framework 행의 깊이가 permit으로 정해지는 것은 이 설계의 결과이며 규격 §5.2대로
깊이와 함께 읽는다.

**결정 F2 — framework 메시징 call에 동기 blocking 종결자를 추가한다.** §4의 "동기 `TrySubmit` 계열을 제공하지 않는다"는
nonblocking try를 막은 것이고 blocking 동기 종결자는 별개다. nonblocking 완료를 주는 유일한 대안은 callback인데 복잡하고 혼동을
주므로 동기 종결자는 **blocking만** 제공한다. 이름은 binding 정책 `bindings/doc/spec/async-coroutine-policy.ko.md` §6을 따른다.

| 언어 | 비동기(현행) | 동기 blocking(추가) |
|---|---|---|
| Java·Node | `submit()` | `submit_sync()` |
| Kotlin | 전용 wrapper `await()` | 추가 없음(Java 표면의 `submit_sync()` 그대로 노출) |
| .NET | `Async()` | `Submit()` |
| C++ | `async()` | `submit()` |

**규칙 F2-a — blocking 종결자는 runtime 실행 문맥에서 부를 수 없다.** handler turn·Spot turn·state lane 위에서 blocking하면 gate를
쥔 채 완료를 기다려 교착한다. 스펙 06 §5(반환 전 완료 보장)·02(handler turn)와 같은 원칙으로, runtime 실행 문맥에서 동기
종결자를 부르면 `InvalidOperation`으로 즉시 실패한다. 동기 종결자의 용도는 application thread(main·테스트·스크립트)다.

**framework 스펙 수정 대상 (G1에 포함, ko+en, 감독자)**

| 파일 | 절 | 수정 |
|---|---|---|
| `01-submit-and-completion.ko.md` | §2 terminator 표 | 동기 blocking 종결자 행 추가: "application 결과(또는 one-way admission)까지 호출 thread를 막는다; runtime 실행 문맥에서는 `InvalidOperation`" |
| 같은 파일 | §4 | "동기 `TrySubmit` 계열을 제공하지 않는다" → "nonblocking try 계열을 제공하지 않는다. 동기 blocking 종결자는 §2 표와 같다"; F2-a 규칙 |
| 같은 파일 | §5 | 변경 없음(`Backpressured`는 public result가 아님 유지). "Core가 재시도를 소유하고 operation별 completion awaitable을 완료한다"에 "(binding 결과 객체의 `admitted`)" 명시 |
| 같은 파일 | §15 | "즉시 backpressure 관찰은 `DONTWAIT` sync terminal이 유일한 표면" 예외 삭제. framework는 binding async terminal의 결과 객체를 소비: `result`로 즉시 판정, `BACKPRESSURED`면 `admitted`를 기다림, request는 `reply` |
| 같은 파일 | §16 | 표에 동기 blocking 열 추가; C++ 문단("blocking `submit()`과 coroutine terminal을 함께 제공하지 않는다") 삭제하고 `submit()`+`async()` 둘 제공으로 |
| `../languages/{dotnet,java,kotlin,node,cpp}/interfaces/*` | messaging call 시그니처 | 동기 종결자 추가, F2-a 오류 |
| `08-messaging-hot-path.ko.md` | E2(제출) | binding 종결자 결과 객체 소비 문구(hop 수 불변) |

**framework 코드 수정 대상 (G5)**

| 언어 | 내부 소비 | 동기 종결자 |
|---|---|---|
| java | `ZLinkJavaRawServicePort.java:211-219` 등 `router.request/send` 호출부 → `.reply()`/`.admitted()`; 3단계 backpressure가 `result`를 사용 | `ZLinkSendCall`·`ZLinkRequestCall`에 `submit_sync()`; runtime 문맥 검사 |
| dotnet | `TrySubmit()` 호출부 2곳(`ZLinkBackendStreamSocketWrapper.cs:356`, `ZLinkManagedMeshNode.cs:12447`) + raw 호출부 → `Async()` 결과 객체 | `Submit()` blocking; 문맥 검사 |
| node | raw 호출부 → 결과 객체 | `submit_sync()`; 문맥 검사 |
| cpp | raw 호출부 → `send_submission_t`/`request_submission_t` | `submit()` blocking; 문맥 검사 |

G5 게이트: framework 테스트·cross-language e2e 통과 + F2-a 회귀 테스트(handler turn 안에서 `submit_sync()` → `InvalidOperation`) 4언어.

## 7. 검증·측정 (G6)

- contract test 7언어(성공 기준 1).
- bindings perf multi: 전 패턴 × tcp × {64,1024,4096,65536}, runs=3 중앙값, 전후 비교. 판정 기준은 D-B 캠페인과 같다(사이즈별 −5%, gate geomean 하락 시 개선 대상).
- ROUTER_ROUTER_REQREP clients=1: 깊이 1 탈출 확인(성공 기준 2).
- gRPC 벤치 Java 3-run(성공 기준 3). 모두 `scripts/perf/perf-ticket.sh submit -p 1`.

## 8. 완료 기준

1. draft §8 네 항목 충족.
2. `bindings/doc/spec/**`·guide·perf 정책·bench 규격에 옛 종결자 시그니처 0건(`check_public_surface`류 grep).
3. 릴리스 노트에 breaking 항목 기록, 버전 CORE.N+1.
4. `decisions.ko.md`에 FB 항목으로 결정·측정 기록.

## 9. 진행 로그

- 2026-09-10: 사용자 결정으로 draft·plan 작성. G0 결정 반영(§2: 정책 §6 이름 유지, `TrySubmit` 제거, 같은 모양 원칙). Go 형태만 확인 대기.
- 2026-09-10: Go 결정 — `Submit(ctx)` 즉시 반환 + `Result()`/`Admitted(ctx)`/`Reply(ctx)` 대기 메서드(§2 #5). G0 결정표 완료.
- 2026-09-10: framework 검토 결정 F1(공개 terminal은 backpressure 미노출, 내부 구현만)·F2(동기 blocking 종결자 추가, 이름은 binding 정책 §6)·F2-a(runtime 문맥에서 호출 금지). §6.1.
- 2026-09-10: #86 CI 재실행 결과 — framework-dotnet 단위 테스트 4플랫폼 588건 실패. 원인은 코드가 아니라 구성:
  `DllNotFoundException: Loaded zlink library is missing required export 'zlink_publish'`. 워크플로가 `VERSION`의
  `LIBZLINK_VERSION=0.17.5` **릴리스 아카이브**를 내려받아 쓰는데 PR의 바인딩은 새 whole-message export를 요구한다
  (`.github/workflows/framework-dotnet.yml:87-99, :205-213`). Core ABI가 깨지는 변경이므로 Core 버전을 올리고 릴리스를
  낸 뒤에야 framework CI가 초록이 된다(버전 정책: Core MAJOR.MINOR). 이 plan의 G0 전제(#86 머지)에 "Core 버전 결정·릴리스"가 붙는다.
- 2026-09-10: Issue 등록 — G1 #88, G2 #89, G3 #90, G4 #91~#96, G5 #97~#100, G6·G7 #101. **milestone 0.18.0**(사용자: 1.0.0이 아니라 0.18.0에 싣는다). Core 태그는 #102 뒤 먼저, binding 태그는 캠페인 뒤.
