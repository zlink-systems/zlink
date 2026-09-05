# Node STREAM Actor bind Stage 2 round 2 결과

승인된 **B 수정 완료**. Durable sender가 bind deadline의 종료를 단독으로 결정한다.
3행 계약 행렬, 기존 bind·registry 테스트, typecheck, 수정 파일 lint,
`verify:m6b-runtime`, TicTacToe.Ts·SupportChat.Ts 각 1회가 통과했다.
남은 실패와 BLOCKERS는 없다. Commit은 하지 않았다.

## 필수 판정

- 소유 계층: Framework durable lifecycle sender가 operation identity, replay, admission 이력과 deadline 소진 종류를 소유한다. Registry는 pending identity·capacity·완료·취소를 관리하고, Core·binding은 물리 handover와 typed submit/request 결과를 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680`의 sender replay·전체 remaining deadline·소진 종류와 `core/doc/spec/core/socket/README.ko.md:160-170`의 §4 handover 즉시 `REQUEST_NOT_CONNECTED` 계약(`bb730c654f`).
- 교차언어 대조: 승인된 parity 진단과 C++ `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:193-298`를 대조했다. C++의 기존 sender는 operation·correlation·request bytes·remaining deadline을 함께 관리한다. Node의 별도 registry timer는 구조적 차이이며, 이번 수정은 그 중복 소유를 제거한다. 다른 언어의 진행 중 수정 결과를 Node 검증 결과로 간주하지 않는다.
- 변경 분류: **B — 기존 결함 수정**, 감독이 승인한 round-1 BLOCKERS의 deadline 소유권 보완과 STREAM bind replay 구현.

## Diff

아래 축약 경로의 기준은 `framework/languages/node/`다.

| 변경 파일 | 결과 |
| --- | --- |
| `packages/framework/src/runtime/foundation/operation-registry.ts:69` | `reserve(timeoutMs, 'sender')`는 timeout 검증·capacity·identity를 유지하고 registry timer를 등록하지 않는다. 기존 기본 예약의 timer는 유지한다. |
| `packages/framework/src/runtime/foundation/service-stateful-registry.ts:622` | 기존 terminal registry 예약이 timeout 소유자 선택을 전달한다. |
| `packages/framework/src/runtime/foundation/service-stateful-runtime.ts:1623` | Bind가 시작 시 정한 deadline과 한 번 encode한 header를 유지한다. 기존 `requestUserSpotOperation`의 루프를 `requestDurableOperation`으로 추출해 create와 bind가 공유한다. Bind의 준비 실패도 pending을 종료한다. |
| `packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:2481` | Sender가 결정한 Framework 오류를 기존 `internalFrameworkWireReply`로 completion에 전달한다. |
| `packages/framework/src/runtime/streams/managed-stream.ts:213` | `service.bindActor()`를 한 번 호출한다. 외부 retry loop와 admission 대기 helper를 제거하고 완료 오류는 기존 `wireReplyFailureException`으로 변환한다. |
| `test/contract/stream-actor-bind-replay.test.js` | 실제 sender·STREAM service·backend mapper·completion table을 연결한 3행 행렬과 추가 회귀 2개를 검증한다. Transport 결과와 ready dispatch는 테스트가 제공한다. |
| `test/contract/stream-runtime.test.js:134` | 기존 첫 submit 실패를 durable sender의 transport에 주입한다. Transport attempt 2회, public bind 1회, 동일 header와 authority 전달을 검증한다. 별도 synchronous service 실패는 같은 오류로 즉시 전달하고 재호출하지 않음을 검증한다. |
| `test/m6b/m6b-runtime.contract.ts:4014` | Sender 소유 예약의 timer 부재, capacity 유지, 정확히 한 번 완료, cancel·shutdown 정리를 검증한다. 기존 registry timeout 테스트도 유지한다. |

Registry timer가 sender의 소진 판단을 호출하도록 위임하는 대안과 timer를 등록하지 않는
대안을 비교했다. 전자는 registry와 sender 사이에 admission 판단을 연결하는 별도 배선이
필요하다. 승인된 후자를 적용해 admission 이력을 sender 안에만 유지했다.

Bind sender는 `ZLinkBackendResultError.operation`의 submit/request 구분으로 admission을
판단한다. Request `NotConnected`는 handover된 admitted request로 계산한다. 재전송할 수
없는 typed 결과와 untyped 오류는 그대로 전달하며, 받은 envelope의 decode는 루프 밖에서
수행한다. 각 attempt는 원래 deadline의 남은 시간을 전부 사용한다. Deadline 분할·증가,
새 poller·retry 상태 테이블, application request 재전송은 추가하지 않았다.

## 계약 행렬

| 상황 | 결과 | 검증한 경계 |
| --- | --- | --- |
| 80ms 전체 deadline 동안 route absent | **PASS — Unavailable** | Typed submit `NotConnected`, target ingress 0회, 동일 header 재전송, public bind 호출 1회 |
| admitted request의 reply withheld | **PASS — DeadlineExceeded** | Attempt에 남은 deadline 전체를 전달, ingress 1회, typed request `TimedOut` 뒤 소진 |
| handover `NOT_CONNECTED` 뒤 route ready | **PASS — 성공** | 200ms 원래 deadline 내 attempt 2회, 동일 correlation·binding generation·header, fixture terminal record 재사용, 성공 뒤 재전송 없음 |

추가로 request `NotConnected` 뒤 submit route absence가 계속되어도 `DeadlineExceeded`로
종료하는 것과, 실패 envelope·malformed reply를 받으면 재전송하지 않는 것을 검증했다.
Completion table의 late/unknown 진단도 0회다.

행렬의 handover는 수정된 Core 계약의 typed marker를 주입한 Framework 계약 테스트다.
진행 중인 Core 구현의 실제 pipe 교체·즉시 completion 자체를 검증한 결과는 아니다.
Target 실행 1회는 fixture의 terminal record를 재사용한 결과이며, 실제 target 중복 처리와
binding generation 회귀는 기존 M6B suite가 별도로 검증한다.

## 검증 결과

모든 실행의 cwd는 `framework/languages/node`이며,
`TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` unset,
`flock -w7200 /tmp/zlink-node-gate.lock`을 적용했다.

| 검증 | 결과 | 로그 (`/dev/shm/zlink-tmp-node/` 기준) |
| --- | --- | --- |
| STREAM 계약 156개 + 새 bind 계약 5개 | **161/161 PASS**. 기존 첫 실패 주입 보강 후 해당 STREAM 파일 156/156 재확인 | `stage2-bind-r2-focused-final.log`, `stage2-bind-r2-existing-stream-final.log` |
| 기존 bind·registry·create replay focused 검사 | **12/12 PASS** | `stage2-bind-r2-registry-bind.log` |
| M5 registry capacity·reply/timeout/shutdown | **2/2 PASS** | `stage2-bind-r2-m5-registry.log`, `stage2-bind-r2-m5-completion.log` |
| `npm run typecheck` | **PASS** | `stage2-bind-r2-typecheck.log` |
| 수정 파일 ESLint | **오류 0**. M6B `.ts` 테스트는 기존 ESLint 설정 대상 밖이라는 경고 1개; M6B 컴파일·실행 통과 | `stage2-bind-r2-lint.log`, `stage2-bind-r2-lint-final-test.log` |
| `npm run verify:m6b-runtime` | **113/113 PASS**, 전체 gate 1회 | `stage2-bind-r2-m6b.log` |
| TicTacToe.Ts | **PASS**, 1회 | `stage2-bind-r2-tictactoe.log` |
| SupportChat.Ts | **PASS**, 1회 | `stage2-bind-r2-supportchat.log` |
| 변경 파일 whitespace 검사 | **PASS** | `git diff --check -- framework/languages/node` |

Sample은 기존 runner를 `--keep-run-dir`로 실행했다. 기존 `messageFlow('normal')`와
OTel file exporter의 로그를 다음 디렉터리에 보존했다.

- TicTacToe.Ts: `/dev/shm/zlink-tmp-node/zlink-tictactoe.ts-nExfSW/logs/`, `flow/flow-api.log`, `flow/flow-play.log`.
- SupportChat.Ts: `/dev/shm/zlink-tmp-node/zlink-supportchat.ts-mFEvSr/logs/`, `flow/flow-api.log`, `flow/flow-session.log`, `flow/flow-support.log`.

## BLOCKERS

**없음.** 요청된 Node 변경과 검증을 완료했다. Core·binding 결함은 관찰되지 않았다.
Core·binding·다른 언어·보호 문서와 기존 사용자 변경은 수정하지 않았으며,
local package 재생성·commit은 수행하지 않았다.
