# Node ClientServer deadline과 shutdown terminal 수정 결과

F-R5-9와 F-R7-4를 Node Framework에서 수정했다. 두 원인은 파일 단위로 분리할 수 있으며
commit하지 않았다. Core, binding, 다른 언어와 보호 문서는 변경하지 않았다.

## F-R5-9 — 호출 deadline

- 소유 계층: Framework의 Channel 송신 operation이 deadline을 소유한다. Socket registry는
  기존 admission 결과와 선택 가능 여부를 관찰한다.
- Spec 조항: `02-channel-transport/02-channel-messaging.ko.md` §3.2·§10,
  `03-spot-actor/04-actor-model.ko.md` §8.1. 경과 시간은 D-095에 따라 monotonic clock을 사용한다.
- 교차언어 대조: .NET `ZLinkClientServerClientRuntime.cs:232`의 시작 timestamp와
  `:251`의 잔여 시간 계산을 확인했다. Node의 채널 기본 timeout 대기와 원래 timeout 재전달이
  원인이며, 언어 구조 때문에 다른 deadline 정책이 필요한 상황은 아니다.
- 변경 분류: **B — 기존 결함**. D-108과 감독 작업 지시에서 승인한 범위다.
- 수정 전/후 규칙 수: ready 대기와 request의 독립 예산 **2 → 호출 deadline 1**.
  Ready 대기의 5초 상한은 유지한다.

`channel-outbound-operations.ts:161`에서 `performance.now()`로 호출 deadline을 고정하고
`channel-socket-registry.ts:646`에 전달한다. Ready 대기 뒤 만료된 호출은 기존 no-ready
결과로 끝나며 backend request를 제출하지 않는다. 미등록 후보는 `NotFound`, 알려진 후보가
선택 불가하면 `Unavailable`이라는 기존 분류도 유지한다.

Request envelope과 `dealer.request`에는 잔여 시간만 전달한다. Binding의 정수 millisecond
계약에 맞춰 잔여 시간을 내림하므로 deadline이 늘어나지 않는다. Registry의
`clientServerReadyWaitBoundMs` helper는 제거했다. 호출별 timeout 인자가 없는 one-way
send와 timeout을 생략한 내부 호출에는 기존 채널·host 기본값 조회가 여전히 필요하므로
registry의 기본값 경로에 남겼다.

시작 시각과 timeout을 묶어 전달하는 대안과 absolute monotonic deadline 하나를 전달하는
대안을 비교했다. 후자는 별도 자료형 없이 잔여 시간을 계산할 수 있어 채택했다.

분리할 diff:

- `framework/languages/node/packages/framework/src/runtime/channels/channel-outbound-operations.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts`
- `framework/languages/node/test/contract/client-server-location-runtime.test.js`

회귀 테스트 6건은 200ms 호출의 미등록·weight-zero 후보, 정확히 100ms 뒤 admission과
남은 100ms 전달, 실제 늦은 admission 뒤 원래 deadline 만료, 만료 시점에 선택된 후보의
미제출, 60초 호출의 5초 ready 상한을 검증한다. 100ms 경계는 제어한 monotonic clock으로
정확히 단언하고, 실제 시간 검증은 `performance.now()`를 사용한다. 기존 assertion은 완화하지 않았다.

## F-R7-4 — 첫 shutdown terminal

- 소유 계층: Framework host가 publication·owner cleanup의 첫 terminal을 소비한다.
- Spec 조항: `05-location-relocation/05-host-relocation-flow.ko.md` §14 step 2·5·6.
- 교차언어 대조: main의 .NET `ZLinkFrameworkDrainExecutor.cs:518`·`:532`·`:545`에서
  publication의 false·예외와 cleanup 예외를 첫 실패로 분류하는 구현을 확인했다.
  Node에 남아 있던 재제출 루프를 같은 계약으로 맞췄다.
- 변경 분류: **B — 기존 결함**. D-107과 감독의 cause 2 작업 지시에서 승인한 범위다.
- 수정 전/후 규칙 수: 성공 시 전진·실패 시 시간 대기 후 재제출 **2 → 첫 terminal 소비 1**
  (publication과 owner cleanup에 동일하게 적용). Retry helper **1 → 0**.

`host/index.ts:1880`의 publication `while`과 `:1891`의 cleanup `for (;;)`를 제거했다.
Publication이 false이면 명시적인 실패 원인을 만들고, throw이면 원래 예외를
`ZLinkDrainingStatePublishError.cause`로 보존한다. Cleanup 예외는
`ZLinkOwnerCleanupError.cause`로 보존한다. 기존 내부 실패 분류와 공개
`ForceStopped/TeardownFailed` 결과를 사용하며 `waitForDrainRetry`는 제거했다.

새 결과 자료형을 추가하는 대안과 기존 오류·결과 계약을 재사용하는 대안을 비교해
기존 계약을 선택했다. Public API는 추가하지 않았다.

분리할 diff:

- `framework/languages/node/packages/framework/src/runtime/host/index.ts`
- `framework/languages/node/test/contract/store-failure-graceful-drain.test.js`

회귀 테스트 3건은 publication false·throw와 cleanup throw에서 호출이 각각 한 번이고,
원인과 기존 teardown 실패 결과가 보존되며 shutdown이 polling interval 1초보다 먼저
끝나는지 검증한다. 성공 후 callback·cleanup 순서를 검증하는 기존 테스트도 유지한다.

## 검증 결과

- TypeScript build와 실제 Node ClientServer 왕복을 포함한 집중 테스트: **12/12 통과**.
- 새 회귀 테스트 9건과 기존 shutdown 성공 테스트 1건: **10/10 × 5회 통과**.
- HEAD의 원래 runtime을 임시 메모리 loader로 사용한 회귀 검증: **8건 실패**.
  기존 200ms 호출은 약 5초 또는 304ms 뒤 종료됐고, publication false와 cleanup 실패는
  약 2.5초 shutdown deadline까지 반복했다. 작업 파일을 되돌리지 않고 검증했다.
- 지정된 `flock -w7200 /tmp/zlink-node-gate.lock npm test`: build·typecheck·lint 통과.
  Runtime 테스트 **1674건 완료, 1673 통과, 1건 실패**, exit 1.
  실패는 `sample-regression.test.js:2311`이 내부에서 실행한 `samples/run_samples.sh`를
  작업 지시의 sample runner 실행 금지에 따라 중단한 결과다. Runner 호출을 확인하기 전에는
  gate 내부 실행을 발견하지 못했다. 이 항목의 assertion이나 실행 조건은 변경하지 않았다.
  그 외 runtime 테스트에는 실패가 없다. 전체 gate에 포함된 sample 실행 허용 여부를
  감독에게 확인 요청했다.
- `flock -w7200 /tmp/zlink-node-gate.lock npm run verify:m6a-runtime`: **41/41 통과**, exit 0.
- `git diff --check`: 통과.

로그 위치: `/tmp/zlink-node-r5-ready-wait-deadline/`. 회귀 반복은
`final-repeat-1.log`부터 `final-repeat-5.log`, 기존 runtime 검증은 `baseline-verified.log`,
전체 gate는 `npm-test.log`, M6A는 `m6a-runtime.log`에 보존한다.

남은 실패: 실행 범위 제한에 따라 중단한 sample runner 계약 테스트 1건.
전체 `npm test`의 0-failure gate는 아직 충족하지 않았다.
