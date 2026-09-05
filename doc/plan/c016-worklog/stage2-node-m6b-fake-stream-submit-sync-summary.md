# Node M6B fake STREAM `submit_sync()` Stage 2 결과

## 결론

승인된 Class A fixture 수정은 적용했다. `createFakeStream()`의 반환형을 `unknown`에서
`Pick<StreamSocket, 'send'>`로 바꾸고, 현재 binding의 `SendSubmitOperation`과 같은
`submit(): Promise<void>` 및 `submit_sync(): void` terminal을 구현했다. 이전 계약의
`flags()`와 `trySend()`는 제거했다. 정상 terminal은 누적한 payload를 `delivered`에 기록하고,
연결 종료와 backpressure는 실제 terminal failure와 같이 예외로 보고한다.

TypeScript compile과 첫 정상 send, backpressure send까지는 통과했다. 그러나 backpressure
예외를 받은 runtime의 catch-all이 session target을 삭제한다. 따라서 상태를 복원한 다음 send가
`InvalidState(8)`이 되어 기존 assertion이 실패한다. 승인 범위가 runtime 수정을 금지하므로 이
문제는 우회하지 않았고 Stage 2 전체 완료의 blocker로 남겼다.

## Diff

- `framework/languages/node/test/m6b/m6b-runtime.contract.ts`
  - public binding package에서 `StreamSocket` type을 import했다.
  - fake 반환형을 `Pick<StreamSocket, 'send'>`로 제한해 실제 사용 표면과 builder terminal을
    TypeScript가 검사하도록 했다.
  - payload 기록을 `submitPending()`에 모으고 async `submit()`과 sync `submit_sync()`가 같은
    성공·실패 동작을 사용하도록 했다.
  - 현재 `StreamSocket`에 없는 기존 `flags()`와 `trySend()` 복제를 제거했다.
- Assertion, timeout, runtime source는 변경하지 않았다.

## 검증 결과

모든 실행은 `framework/languages/node`에서 `TMPDIR=/dev/shm/zlink-tmp-node`를 사용하고
`ZLINK_LIBRARY_PATH`를 unset했으며, 각 npm/test invocation을
`flock -w7200 /tmp/zlink-node-gate.lock`으로 감쌌다.

### 대상 test 3회

| 실행 | 결과 | test 시간 | 실패 지점 |
|---|---|---:|---|
| 1 | fail | 20.852 ms | backpressure 복원 뒤 send: `8 !== 0` |
| 2 | fail | 21.779 ms | backpressure 복원 뒤 send: `8 !== 0` |
| 3 | fail | 22.322 ms | backpressure 복원 뒤 send: `8 !== 0` |

세 실행 모두 첫 정상 send의 `Ok`와 payload 1회 기록, backpressure send의 `InvalidState`와
추가 payload 없음 assertion을 통과했다. 실패는 그 다음 `streamState.backpressured = false`로
복원한 send의 `SubmitResult.Ok` assertion에서만 발생했다
(`m6b-runtime.contract.ts:5562-5570`). 결과는 **0/3 pass**로 결정적이다.

### 이전 flake test 5회

| 실행 | 결과 | test 시간 |
|---|---|---:|
| 1 | pass | 374.484 ms |
| 2 | pass | 378.869 ms |
| 3 | pass | 382.320 ms |
| 4 | pass | 376.446 ms |
| 5 | pass | 376.425 ms |

`remote User Spot target executes once and rewrites correlation on terminal replay`는 단독
**5/5 pass**였다. 이번 표본에서는 결정적으로 통과했지만, 이전 aggregate의 1회 실패가 있으므로
flake가 해소됐다고 판정하지 않는다.

### 전체 M6B gate

`npm run verify:m6b-runtime`은 TypeScript compile을 통과한 뒤 **111 pass / 1 fail**이었다.
유일한 실패는 대상 test의 backpressure 복원 뒤 send이며 test 시간은 14.324 ms였다. replay
test는 전체 gate에서도 380.944 ms에 통과했다.

## Catch boundary 후속 사항

`RawStreamSessionService.deliver()`의 `try`는 payload decode, builder 생성, 모든 `message()` 호출,
`submit_sync()`를 함께 감싼다
(`framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1996-2004`).
인자 없는 `catch`는 예외 종류를 확인하지 않고 session target을 삭제한 뒤 `false`를 반환한다
(`:2006-2011`). 따라서 연결 종료 같은 transport 예외뿐 아니라 다음 예외도 삼킨다.

- 잘못된 fake가 만든 `TypeError`와 같은 programming error
- `decodeApplicationPayloadView()` 또는 multipart decode의 protocol error
- backpressure 또는 timeout과 같이 연결 종료가 아닌 submit terminal error

반환된 `false`는 `ServiceStatefulRuntime.deliverBoundSession()`에서 `protocolError`가 되고
(`service-stateful-runtime.ts:3599-3605`), local ingress는 이를 `SubmitResult.InvalidState`로
축약한다(`:4149-4160`). Catch 주석은 client close만 설명하지만 실제 경계는 non-transport
예외까지 모두 session close로 처리한다. 이는 루트 `AGENTS.md` §3의 금지 패턴인
“예외를 삼키는 catch-all”에 해당하므로 별도 runtime 진단과 A/B 승인 후 수정해야 한다.

## 소유권과 분류

- 소유 계층: 이번 변경은 binding builder 계약을 복제하는 Node M6B test fixture가 소유한다.
- Spec 조항: Node binding의 STREAM `send(routingId)`는 `SendOperation`을 반환하고 terminal은
  async `submit()`과 sync `submit_sync()` 쌍이다. Fixture는 이 exact type에서 표면을 파생한다.
- 교차언어 대조: 승인 진단대로 C++/.NET/Java의 동등 M6B test는 통과하며, Node만 binding
  builder를 수동 복제한 구조적 차이가 있다.
- 변경 분류: **A — 0.17 binding terminal 계약 적응**. Runtime catch-all은 이 변경에 포함하지
  않은 별도 후속 진단 대상이다.

## BLOCKERS

- `node-raw-mesh-backend.ts:2006-2011`의 catch-all이 backpressure terminal exception도 연결
  종료로 오분류하고 `sessionTargets`에서 현재 RID를 삭제한다. 이 때문에 기존 “backpressure 뒤
  복원 send는 `Ok`” assertion을 fixture 변경만으로 만족할 수 없다.
- Runtime source 수정은 이번 승인 범위에서 명시적으로 금지됐다. Assertion 완화, session target
  재주입, fake 전용 재등록은 모두 원인 우회이므로 적용하지 않았다.

