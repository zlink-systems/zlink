# Node STREAM Actor bind Stage 2 결과

상태: **STOP — 승인 진단의 deadline 소유권 보완 필요**. Runtime과 test는 수정하지
않았다. 사용자의 “if the diagnosis turns out incomplete, STOP and report” 지시에 따라
아래 누락 경계를 확인한 뒤 구현을 중단했다.

## Diff

- 추가 파일: `doc/plan/c016-worklog/stage2-node-stream-actor-bind-replay-summary.md`.
- Node runtime·test diff 없음. Core·binding·다른 언어·보호 문서 변경 없음.
- Local package 재빌드와 commit 없음. 시작 branch는 `main`이다.

## 필수 판정

- 소유 계층: Framework durable lifecycle sender가 stable `OperationId`, replay와 admission 이력에 따른 전체 deadline 종료를 소유한다. Binding typed submit/request 결과를 admission 근거로 소비한다.
- Spec 조항: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680` — terminal envelope 부재 시 같은 operation 재전송, attempt마다 남은 전체 deadline 사용, never-admitted=`Unavailable`, admitted-without-reply=`DeadlineExceeded`, application request 자동 재전송 금지.
- 교차언어 대조: 승인 진단의 .NET·Java·C++ 행렬을 확인하고 `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:193-243,268-292`를 대조했다. C++는 기존 sender가 remaining deadline과 route 소진을 처리한다. Node에는 bind pending registry와 create sender의 독립된 deadline 종료가 있어 helper 재사용만으로는 소진 종류를 보존하지 못한다.
- 변경 분류: **A — 승인된 계약 적응 작업**. 이번 결과는 추가 진단이며 runtime 구현은 없다.

## BLOCKERS

승인 진단의 Node 최소 수정은 `service-stateful-runtime`에서 기존 create sender helper를
재사용하도록 요구하지만, bind의 독립된 pending-operation 타이머를 다루지 않는다.
아래 경로는 admission 이력을 확인하기 전에 pending promise를 종료한다.

1. `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:1632`:
   `bindSession()`이 sender 호출 전에 `operations.reserve(timeoutMs)`를 실행한다.
2. `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-registry.ts:622-623`:
   이 예약은 `OperationRegistry.reserve()`로 전달된다.
3. `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:88-90`:
   원래 timeout에 무조건 `OperationTimeoutError`로 끝내는 타이머를 등록한다.
4. `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4021-4045`:
   기존 create sender는 별도로 deadline과 admission 이력을 관리한다. 여기서 계산한
   `Unavailable`은 pending registry가 먼저 종료한 뒤에는 전달할 수 없다.
5. `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:2480-2481`:
   먼저 발생한 `OperationTimeoutError`는 `RequestResult.TimedOut`으로 변환된다.
   따라서 승인된 request-terminal mapper 교체만 적용하면 never-admitted bind도
   `DeadlineExceeded`가 된다.

진단 보완에는 sender의 admission 이력과 registry의 정확한 deadline 종료를 한 소유자에
연결하는 방법이 필요하다. 검토 가능한 대안은 registry의 deadline 종료가 sender의 소진
결정을 소비하게 하거나, registry의 pending 수명 관리를 유지하면서 durable sender가
deadline 종료를 전담하게 하는 것이다. 어느 대안도 이번 작업에서 구현하지 않았다.
Timeout 증가·분할·종료 지연으로 이 경계를 우회하지 않았다.

## 재현 결과

Repository source를 바꾸지 않고 기존 build의 `ServiceStatefulRuntime`을 사용한
메모리 내 probe를 실행했다. Probe는 bind의 `submitRequest` 호출만 기존
`requestUserSpotOperation` 루프로 연결하고 실제 bind pending registry를 유지했다.
Raw request는 typed `ZLinkBackendResultError('submit', SubmitResult.NotConnected)`를
반환한다. 이는 helper를 연결하는 최소 변경의 누락 경계를 조사한 실험이며,
수정된 runtime의 회귀 테스트 통과 결과가 아니다.

- Deadline: 80ms. Native target ingress: 0회.
- Attempt: 4회. Pending 결과: `OperationTimeoutError`.
- Sender 결과: `Unavailable`(enum 값 5).
- Sender가 pending을 완료했는지: `false`.
- Probe 종료 코드: 0. 첫 실행은 probe가 숫자 enum을 문자열과 비교해 실패했으며,
  공개 enum 상수로 비교를 바로잡은 뒤 위 결과를 확인했다.
- Script: `/dev/shm/zlink-tmp-node/stage2-bind-deadline-probe.cjs`.
- Log: `/dev/shm/zlink-tmp-node/stage2-bind-deadline-probe.log`.
- 환경: `framework/languages/node`, `TMPDIR=/dev/shm/zlink-tmp-node`,
  `ZLINK_LIBRARY_PATH` unset, `flock -w7200 /tmp/zlink-node-gate.lock`.

## 행렬과 검증

| 계약 행 | 결과 |
| --- | --- |
| 전체 deadline 동안 route absent → `Unavailable`, ingress 0회 | 최소 helper 연결 probe에서 registry가 먼저 timeout으로 종료함을 확인. 회귀 구현 차단. |
| admitted 뒤 reply 없음 → `DeadlineExceeded` | 미실행 — 누락된 deadline 소유권 진단에서 중단. |
| handover 1회 timeout → 동일 operation/header/generation 재전송, terminal 뒤 종료 | 미실행 — 같은 사유. |

Touched test 파일은 없다. `npm run typecheck`, touched-file lint,
`npm run verify:m6b-runtime`, TicTacToe.Ts·SupportChat.Ts 각 1회 검증은
구현 중단 지시에 따라 실행하지 않았다. 남은 문제는 위 BLOCKER이며,
요청한 Stage 2 구현과 회귀·최종 검증은 완료되지 않았다.
