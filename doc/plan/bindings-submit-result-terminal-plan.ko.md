# 바인딩 submit 종결자 결과 enum — 적용 plan

> 설계: [`../draft/bindings-submit-result-terminal.ko.md`](../draft/bindings-submit-result-terminal.ko.md).
> 사용자 결정(2026-09-10): 호환성 없이 변경, 7언어 동시, perf는 `BACKPRESSURED`일 때만 비동기 대기.
> 선행: PR #86(Issue #63 whole-message API) 머지 뒤 그 위에서 시작한다.
> 작업 방식: `doc/principal/dev/development-workflow.ko.md`. 스펙·정책·문서는 감독자, 코드는 codex job, 측정은 `perf-ticket.sh`.

## 0. 범위·전제

- 대상: `bindings/{cpp,dotnet,go,java,node,python,rust}` 공개 send·request 비동기 종결자, 그 내부 completion owner,
  contract test, perf multi·single 클라이언트, gRPC 벤치 raw 드라이버 4언어, framework 4언어의 binding 호출부, 스펙·정책·guide.
- 비대상: C API·Core(변경 없음), `submit_sync()`, publish, reply, STREAM.
- 기준 트리: PR #86 머지 커밋. binding 버전은 다음 릴리스에서 CORE.N+1로 올린다(`project-versioning-policy`).

## 1. 반영 순서 (단계 게이트)

| 단계 | 내용 | 담당 | 게이트 |
|---|---|---|---|
| G0 | #86 머지. 이름 확정: `SendSubmission`/`RequestSubmission`, `result`/`admitted`/`reply`; Go 비동기 종결자 이름; .NET `TrySend` 유지 여부 | 사용자·감독자 | 이 문서 §2 결정표 채움 |
| G1 | 스펙·정책 문안(§3) 커밋 | 감독자 | 표·문장 diff 검토 |
| G2 | **Java 파일럿**: 종결자·CompletionOwner·contract test·Kotlin 확장 | job `java-submit-result` | contract 통과, 성공 기준 1 |
| G3 | Java perf multi/single reqrep·sendsend 클라이언트를 §5 규칙으로; gRPC 벤치 Java raw 드라이버 | job `java-perf-submit-result` | 성공 기준 2·3 |
| G4 | 나머지 6언어 병렬(cpp·dotnet·node·go·rust·python): 종결자·owner·contract test·perf 클라이언트 | job 6개(동시 ≤5, `env-job-concurrency-cap`) | 언어별 contract·perf 통과 |
| G5 | framework 4언어 호출부 `.reply()` 전환, samples·guide 코드 블록 | job 4개 또는 G4 job에 포함 | framework 테스트·e2e(cross-language) |
| G6 | 전체 perf 재측정(multi 전 패턴, 4 size, tcp) 전후 비교; gRPC 벤치 Java 3-run | 감독자 ticket | 성공 기준 4 |
| G7 | 릴리스 노트(breaking), 문서 검사(`check_doc_links`, `check_prose_neutrality`) | 감독자 | 완료 기준 |

G2 결과가 설계를 바꾸면 G4 전에 draft를 갱신한다. G4 언어별 job은 G2 커밋을 참조 구현으로 받는다.

## 2. 먼저 정할 것 (G0 결정표)

| # | 항목 | 결정 (2026-09-10, 사용자) |
|---|---|---|
| 1 | 결과 객체 이름 | `SendSubmission`/`RequestSubmission` (draft §4) |
| 2 | 필드 이름 | `result`, `admitted`, `reply` (언어 관례 대소문자) |
| 3 | 즉시 실패의 전달 | 예외/에러 유지(draft §3.1) |
| 4 | 종결자 이름 | **`bindings/doc/spec/async-coroutine-policy.ko.md` §6 표 그대로.** 새 이름을 만들지 않는다. 비동기 종결자(Java·Node·Python·Rust `submit()`, .NET `Async()`, C++ `async()`)의 반환형만 결과 객체로 바꾼다. 동기 종결자(`submit_sync()`, .NET·C++ `submit()`) 불변 |
| 5 | Go | 정책 §6의 Go 종결자는 `Submit(context.Context)` 하나(동기). 제안: `Submit(ctx)`가 `(RequestSubmission, error)`를 돌려주고 `Reply`는 채널로 받는다(`<-sub.Reply`가 지금의 블로킹 결과). 정책 §6 Go 행을 이렇게 고친다. **확인 필요** |
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
| go | `internal/native/operations.go`, `root_projection.go` | writable retry 경로 | `writable_retry_test.go` 확장 |
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
- 2026-09-10: #86 CI 재실행 결과 — framework-dotnet 단위 테스트 4플랫폼 588건 실패. 원인은 코드가 아니라 구성:
  `DllNotFoundException: Loaded zlink library is missing required export 'zlink_publish'`. 워크플로가 `VERSION`의
  `LIBZLINK_VERSION=0.17.5` **릴리스 아카이브**를 내려받아 쓰는데 PR의 바인딩은 새 whole-message export를 요구한다
  (`.github/workflows/framework-dotnet.yml:87-99, :205-213`). Core ABI가 깨지는 변경이므로 Core 버전을 올리고 릴리스를
  낸 뒤에야 framework CI가 초록이 된다(버전 정책: Core MAJOR.MINOR). 이 plan의 G0 전제(#86 머지)에 "Core 버전 결정·릴리스"가 붙는다.
