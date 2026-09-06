# Node R6 parity 수정 결과 — F-R6-4·F-R6-5·F-R6-6 / B3

2026-09-06. 세 원인의 구현과 신규 회귀 테스트 47건을 추가했다. 신규 테스트는 최종 코드에서 각각
5회 통과했다. **요청한 전체 npm gate의 0-failure 완료 판정은 보류한다.** Gate 내부 sample 실행이
작업 지시와 충돌했고, 추가 M6A 검증에는 아래 BLOCKERS의 잔여 실패가 있다. Commit은 하지 않았다.

진단·승인 근거는 D-105와 이번 감독 작업 지시다. 원인 위치는 수정 전 코드 기준이며, 수정 위치는
현재 작업 파일 기준이다. 아래 상대 경로의 기준은 `framework/languages/node/`다.

## F-R6-4 — Host seal과 mesh admission 연결

- 소유 계층: Framework host가 admission seal을 소유하고 raw mesh가 그 값을 조회한다.
- Spec: `05-location-relocation/05-host-relocation-flow.ko.md` §14 step 1. Seal 뒤에는 outbound
  Hello와 inbound Hello의 Admit을 막고, admitted peer에는 Draining Update를 보낸다.
- 원인: `packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:695`의 빈 frame
  응답과 `:714`의 Hello admission에 host seal 조회가 없었다. `announcePeer`·`announceExpectedPeers`도
  seal을 조회하지 않았다. `runtime/host/index.ts:1784`의 종료 게시는 채널 weight만 바꾸어 raw descriptor는
  `serving`으로 남고, 채널이 없는 mesh에는 Update도 없었다.
- 수정: Host의 기존 `admission.accepts(meshName)`를 manager → backend adapter → raw runtime에
  callback으로 전달했다. Raw의 announce·빈 frame·inbound Hello에서 동일 seal을 조회한다.
  Backend `publishDraining()`은 기존 descriptor 게시 경로에 Draining 상태와 채널 weight 0을 전달한다.
  `updateLocalWeights`를 `updateLocalDescriptor`로 바꾸어 기존 Update 인코딩·전송 루프를 재사용했다.
  이미 admitted된 peer의 inbound Update 처리는 계속 진행한다.
- 교차언어: .NET `ZLinkManagedMeshNode.cs:8078,8482`, Java `ZLinkJavaRawMeshNode.java:6353,6919`의
  seal 조회와 대조했다. Node에는 host와 raw runtime 사이의 조회 배선이 빠져 있었다.
- 변경 분류: **B — 기존 결함**, D-105 승인 범위.
- 대안: 별도 mesh 종료 flag를 저장하는 방식과 기존 host seal을 조회하는 방식을 비교하여 후자를 선택했다.
- 수정 전/후 규칙 수: admission 허용 판정 **2 → 1**. Host와 raw의 분리된 판단을 host seal 하나로 통합했다.
- 회귀 테스트: `test/contract/mesh-shutdown-admission-seal.test.js`의 2건. 실제 host shutdown을 시작하고
  accepted work를 유지한 상태에서 신규 Hello·빈 frame·announce를 검사한다. 채널 유무 모두 신규 peer가
  admission되지 않고, 기존 peer에 Draining Update가 한 번 전달되며 inbound Update도 처리되는지 확인한다.
- 검증: 신규 2건 × 5회 통과. 기존 drain·topology contract 52건 통과. 추가 M6A 결과는 BLOCKERS 참조.

Diff 분리 파일 8개:

1. `packages/framework/src/runtime/backend/contracts/index.ts`
2. `packages/framework/src/runtime/backend/node/node-mesh-backend-adapter.ts`
3. `packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts`
4. `packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts`
5. `packages/framework/src/runtime/host/index.ts`
6. `packages/framework/src/runtime/spots/spot-node-runtime-manager.ts`
7. `test/m6a/m6a-runtime.contract.ts` — 기존 메서드 이름 참조 2곳 변경, assertion 유지.
8. `test/contract/mesh-shutdown-admission-seal.test.js`

## F-R6-5 — Remote Actor Join의 durable 송신

- 소유 계층: Operation을 시작한 Framework durable sender가 replay와 deadline을 소유한다.
- Spec: `03-spot-actor/04-actor-model.ko.md` §8.1의 replay 주체·고정 identity·전체 remaining deadline·
  terminal envelope·deadline 단일 소유 규칙, §9의 관찰 요구. Terminal 보존 책임은 membership §2에 있다.
- 원인: `packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4252`의 단일
  `raw.requestService`와 `:4263`의 `submitActorJoin` 연결이 첫 transport failure를 Join 실패로 종료했다.
- 수정: 기존 `requestDurableOperation`의 operation-kind에 `actorJoin`을 추가하고 입력을 frame 배열로
  일반화했다. Remote Join은 한 번 encode한 전체 frame과 동일 correlation/OperationId를 재사용한다.
  네 Join 진입점에서 monotonic deadline을 고정하고 remote pending의 deadline 소유자를 `sender`로 지정한다.
  남은 시간을 각 attempt에 전달하고, 받은 envelope는 replay 루프 밖에서 decode한다. Encode 실패와 terminal
  완료 시 기존 pending 및 canonical handoff bookkeeping을 종료한다.
- 교차언어: .NET `ZLinkDurableRequest.cs`, `ZLinkManagedMeshNode.cs`의 native durable Actor Join 연결,
  `DurableRequestTests.cs`와 `CanonicalActorJoinIngressReplyTests.cs`, Java `ZLinkJavaDurableRequest.java`와 대조했다.
  Node의 multipart Join도 기존 durable sender로 표현할 수 있으므로 별도 재전송 구현은 필요하지 않았다.
- 변경 분류: **B — 기존 결함**, D-105 승인 범위.
- 대안: Join 전용 재시도 루프와 기존 durable sender의 frame 배열 지원을 비교하여 후자를 선택했다.
- 수정 전/후 규칙 수: remote lifecycle 송신 규칙 **2 → 1**. Remote Join deadline 소유자도 registry·sender
  두 곳에서 sender 한 곳으로 정리했다.
- 회귀 테스트: `test/contract/actor-join-durable-replay.test.js`의 18건. Entry/User Spot × canonical/일반
  Join × submit/request 단절 × accepted/rejected terminal의 16조합과 admission 여부별 deadline 2건이다.
  Transport attempt를 통제하여 재연결 뒤 원래 terminal을 반환하고, 실행 1회·동일 전체 frame·동일 OperationId·
  원래 deadline의 전체 잔여 시간·terminal 뒤 추가 송신 없음·handoff 정리를 검증한다.
- 검증: 신규 18건 × 5회 통과. Bind·deferred Join을 포함한 초기 관련 JS 41건 통과.
  최종 관련 JS 187건과 M6B/M6C 142건도 통과했다.

Diff 분리 파일 2개:

1. `packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
2. `test/contract/actor-join-durable-replay.test.js`

## F-R6-6 / B3 — Lifecycle 종료 전달과 typed replay 판정

- 소유 계층: Logical connection intent 소유자가 제거를 결정하고 raw mesh의 기존 expectation·admitted peer
  상태가 lifecycle 종료 사실을 결정한다. Durable sender는 그 전이를 소비하여 operation을 종료한다.
  Native typed error 변환은 Node backend adapter가 소유한다.
- Spec: `03-spot-actor/03-mesh-node.ko.md` §7.1의 intent 제거·terminal·peer 부재 규칙,
  `03-spot-actor/04-actor-model.ko.md` §8.1의 즉시 Unavailable·typed transient replay·deadline 종류 규칙.
- 원인: `packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4071`의 sender는
  runtime open 여부와 deadline만 확인했다. `:4095`는 streamBind에만 typed 판정을 적용했다.
  `runtime/backend/node/node-raw-binding-port.ts:285`는 기존 adapter의 typed error 변환을 거치지 않았다.
- 수정: Raw의 `disconnectPeer`, endpoint intent 제거, NotRequired intent 정리에 lifecycle 전이 통지를 연결했다.
  판정은 `expectedPeers`와 `topology.peer`를 조회하는 한 곳에 있다. Endpoint로 제거할 때 이미 RID로 확정된
  expectation도 같은 전이에서 제거한다. Physical disconnect에는 종료 통지를 추가하지 않았다.
  Sender는 operation 범위의 구독과 기존 `awaitWithAbort`를 사용하여 in-flight request 또는 기존 replay 대기를
  즉시 종료하고 구독을 해제한다. 별도 peer lifecycle/generation cache, monitor, poller, 종료 감시 timer는 없다.
  기존 replay 간격과 deadline은 늘리지 않았다.
- B3 수정: 다섯 operation 종류가 `durableRequestCanReplay` 하나를 사용한다. Typed submit의
  NotConnected·NotFound·NotAdmitted·Backpressured, typed request의 NotConnected·TimedOut만 replay한다.
  Admission 여부도 typed submit/request phase로 판단한다. Raw binding request는 기존
  `translateBindingResultError`를 재사용하여 native error와 cause를 보존한다.
- 교차언어: .NET `ZLinkManagedMeshNode.cs:460,4304`의 intent 제거 event → pending 완료,
  `ZLinkDurableRequest.cs`의 typed 판정, Java `ZLinkJavaRawMeshNode.java:7047`와
  `ZLinkJavaDurableRequest.java`의 lifecycle·typed 판정과 대조했다. Node의 누락된 전이 전달과 오류 변환을 연결했다.
- 변경 분류: **B — 기존 결함**, D-105 승인 범위.
- 대안: Sender가 intent를 별도 polling하는 방식과 기존 제거 전이를 구독하는 방식을 비교하여 후자를 선택했다.
  Operation별 error 분기 대신 공통 typed 판정을 사용했다.
- 수정 전/후 규칙 수: lifecycle 종료 판정 **2 → 1**, replay 오류 판정 **2 → 1**.
- 회귀 테스트: `test/contract/durable-operation-lifecycle.test.js`의 27건. 다섯 operation 각각에 대해
  in-flight·replay 대기의 즉시 종료, physical disconnect 뒤 replay 유지와 endpoint 제거의 terminal 전환,
  typed transient 6종·non-replayable 6종, malformed terminal을 검증한다. Stale lifecycle intent 제거가 현재
  admitted replacement를 종료하지 않는지와 설치된 binding의 실제 route 부재 오류 변환도 검사한다.
- 검증: 신규 27건 × 5회 통과. 기존 User Spot terminal replay 6건, 관련 JS 187건, M6B/M6C 142건 통과.

Diff 분리 파일 14개:

1. `packages/framework/src/runtime/backend/node/node-backend-adapter-support.ts`
2. `packages/framework/src/runtime/backend/node/node-raw-binding-port.ts`
3. `packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts`
4. `packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
5. `test/contract/durable-operation-lifecycle.test.js`
6. `test/contract/actor-join-durable-replay.test.js`
7. `test/contract/deferred-actor-join.test.js`
8. `test/contract/raw-service-mesh.test.js`
9. `test/contract/stream-actor-bind-replay.test.js`
10. `test/contract/stream-runtime.test.js`
11. `test/m6b/m6b-runtime.contract.ts`
12. `test/m6b/m6b-user-spot-terminal-replay.contract.ts`
13. `test/m6c/m6c-actor-join-originate.contract.ts`
14. `test/m6c/m6c-host-relocation.contract.ts`

Test double 58곳에 필수 전이 구독 인터페이스를 추가했다. 기존 terminal replay fixture의 generic Error와
변환 전 native error는 raw port가 실제 반환하는 `ZLinkBackendResultError`로 바꿨다. 미-admission fixture는
typed submit NotConnected를 사용하며 기존 Unavailable assertion을 유지했다. 이 파일의 경과 시간 assertion은
monotonic clock으로 바꿨고, wire deadline은 기존 Unix timestamp를 유지했다. 기존 assertion을 낮추지 않았다.

## Gate 결과와 분리 patch

- Framework build, `tsc -p tsconfig.json --noEmit`, 변경 runtime 파일 ESLint: 통과.
- M6A·M6B·M6C TypeScript build: 통과.
- 최종 신규 회귀: **47/47 × 5회**, failure·cancelled 0.
- 기존 drain·topology JS: **52/52**. 변경된 기존 raw/stream/deferred JS: **187/187**.
- 기존 User Spot terminal replay: **6/6**. M6B runtime·M6C Join/host relocation: **142/142**.
  M6C host relocation의 종료까지 약 174초가 걸렸으며 원본도 24/24, 약 175초로 정상 종료했다.
- 추가 M6A runtime: **37/41**, 원본 비교와 잔여 실패는 아래 참조.
- 요청 명령 `cd framework/languages/node && flock -w7200 /tmp/zlink-node-gate.lock npm test`:
  build·typecheck·lint 통과. 완료된 TAP는 **1066/1066, failure 0**. 이후 지시 충돌로 중단하여 **exit 143**이며
  전체 gate 통과로 판정하지 않는다.

로그는 `/tmp/zlink-node-r6-parity/`에 보존했다. 주요 파일은 `npm-test.log`, `final-regression-1.log`부터
`final-regression-5.log`, `changed-contract-tests.log`, `m6bc-related.log`, `m6b-replay.log`,
`m6a-related.log`, `m6a-baseline.log`, `m6a-not-required-{current,baseline}-*.log`다.

감독의 원인별 commit을 위한 patch는 같은 디렉터리의 `F-R6-4.patch`, `F-R6-5.patch`, `F-R6-6.patch`다.
이 순서로 원본 파일에 적용하면 현재 Node 변경 파일 21개의 내용을 byte 단위로 동일하게 복원함을 검증했다.
공유 파일의 원인별 hunk도 이 patch로 분리되어 있다. 이 보고서는 별도 문서 파일이다.

## BLOCKERS

1. **전체 npm gate와 sample 금지 지시의 충돌.**
   `framework/languages/node/test/contract/sample-regression.test.js:2306`은 `:2311`에서
   `samples/run_samples.sh`를 직접 실행한다. 지정한 npm gate를 실행하자 이 경로가 sample 전체 runner를
   호출했다. 충돌을 확인한 뒤 해당 gate process group에 SIGTERM을 보내 종료했다. Sample 금지 조건을 유지한
   채 원래 명령을 완주할 수 없다. 감독에게 gate 내부 sample 실행 허용 여부를 요청했으며 아직 답변이 없다.
   감독이 전체 gate를 수행하거나 이 경계의 실행을 승인해야 최종 0-failure 판정이 가능하다.
2. **추가 M6A suite의 잔여 실패.** `test/m6a/m6a-runtime.contract.ts`의 `:245`는 monitor fixture에
   `drain`이 없어 실패하고, `:669`는 이전 connection의 명시적 teardown 기대치, `:845`는 paired-lane
   monitor 기대치에서 실패한다. 세 건은 원본 코드와 원본 test를 별도 디렉터리에 복원하여 동일하게 재현했다.
   `:1474`의 Object Client NotRequired test는 수정본 전체 실행에서 1회, 격리 5회 중 1회 실패했다.
   실패 로그에는 한쪽의 NotRequired만 기록되고 반대쪽 관찰이 완료되지 않았다. 원본은 전체 실행에서 통과했고
   격리 25회도 통과했다. **이 간헐 실패의 원인은 미확정이며 사전 존재 실패로 단정하지 않는다.**
   이 작업에서는 해당 admission 기대치·fixture 조건·timeout·runtime 동작을 완화하거나 변경하지 않았다.

세 수정의 분류는 B지만, 위 검증 blocker가 남아 있으므로 commit 가능 판정은 감독에게 맡긴다.
