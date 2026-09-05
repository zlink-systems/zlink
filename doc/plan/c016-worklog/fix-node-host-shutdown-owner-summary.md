# Node host shutdown 소유 계층 수정 결과

2026-09-05. [승인된 BLOCKERS 진단](fix-node-teardown-sigkill-summary.md#blockers)의
Framework 결함 5개와 검증에서 발견한 ZoneWorld runner의 중복 종료, SupportChat의 idle 판정 시각을 수정했다.
**완료 조건은 미충족이다.** 개별 sample 7개와 최종 aggregate는 모두 exit 0이며 cleanup SIGKILL은 없다. `npm test`는 한 번 실행해 **1,590 pass / 1 fail**이다. 남은 실패는 수정 금지 범위인 공유 ZoneWorld UI의 첫 키 입력 race이며, 검토용 owner patch를 준비했다.

`main`에서 작업했다. 기존 Nest shutdown 연결, STREAM async terminal, runner reaping과
회귀 테스트를 보존했다. Core·binding·다른 언어·shared_sample·보호 문서를 수정하지 않았고,
package 재설치와 commit을 하지 않았다. Runner의 500ms grace는 그대로다.

아래 `N/`은 `framework/languages/node/`이며, line은 수정 후 파일 기준이다.

## 결함별 변경

### 1. Draining 게시 뒤 불필요한 대기

Diff: `N/packages/framework/src/runtime/host/index.ts:1878`의 `publishHostDraining()`은
게시 성공 뒤 바로 반환한다. 성공 뒤 polling interval을 기다리던 단계를 삭제했다.

소유 계층: Framework host의 shutdown publication.

Spec 조항: [Host relocation flow §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md#14-shutdown과-relocate의-경쟁), 단계 2→3→4→5. 게시 성공 뒤 별도 시간 대기를 요구하지 않는다. 영문 §14도 대조했다.

교차언어 대조: Java `ZLinkFrameworkRuntime.java:2081`의 `runDrain()`은 `markDraining()` 완료 뒤
barrier와 Spot cleanup을 진행한다. .NET `ZLinkFrameworkDrainExecutor.cs:454`에는
`WaitForDescriptorPropagationAsync()`의 시간 대기가 남아 있다. 이 차이는 §14와 Java를
근거로 판단했으며, 다른 언어 코드는 수정하지 않았다.

변경 분류: **B 기존 결함**, 승인된 BLOCKER 1.

### 2. Actor membership 때문에 시작하지 못하던 Spot 종료

Diff: `N/packages/framework/src/runtime/spots/spot-activation-state.ts:370`의 `canClose(reason)`이
close reason에 따른 자격을 소유한다. HostShutdown은 membership을 유지한 채 닫는다.
`spot-activation-registry.ts:199`와 `spot-activation.ts:766`이 seal 전·후에 같은 규칙을 사용한다.
`requestDrainClose`, departure notification과 별도 drain 상태를 삭제했다.

소유 계층: Framework Spot activation lifecycle.

Spec 조항: Host relocation flow §14 단계 4·5와 cleanup 순서 1. Actor membership과 local
instance가 유효한 상태에서 callback을 호출하고 local scope를 정리한다.

교차언어 대조: Java `ZLinkSpotLifecycle.java:538`의 `closeAllAsync(deadline)`과
`ZLinkInstanceSpotActivation.java:326`의 `close(HOST_SHUTDOWN, deadline)`은 membership 소멸을
선행 조건으로 두지 않는다. .NET `ZLinkFrameworkDrainExecutor.cs:217`도 shutdown에서
Actor handoff 없이 Spot callback→local cleanup을 지시한다. Node의 별도 drain gate가 원인이었다.

변경 분류: **B 기존 결함**, 승인된 BLOCKER 2.

### 3. Detached close의 callback 오류 누락

Diff: `N/packages/framework/src/runtime/spots/index.ts:1102`의 `drainForShutdown()`이 기존
`closeWithReason()` terminal을 모두 기다리고 오류를 반환한다. `spot-activation.ts`의
`scheduleDrainClose()`와 registry의 사용처 없어진 `whenMeshEmpty()`·mesh waiter를 삭제했다.
`spot-closing.ts:26`은 동기 callback throw도 timer 해제의 `finally` 안에서 처리한다.

소유 계층: Framework Spot manager의 close terminal 수집과 shared host의 최종 outcome 판정.

Spec 조항: Host relocation flow §14 단계 6 및 마지막 문단. Callback exception은
`ForceStopped/TeardownFailed`이며, collection 제거만으로 `Stopped`를 확정하지 않는다.

교차언어 대조: Java `ZLinkSpotLifecycle.java:538`은 각 close의 오류를 수집한 뒤 cleanup 전체를
기다리고 실패를 반환한다. `ZLinkSpotRuntime.java:1741`도 Instance·infrastructure 오류를
수집한다. .NET drain executor의 `ForceStopAsync()`도 resource cleanup 오류를 수집한다.
Node의 detached task와 collection-only 관찰을 기존 close terminal로 합쳤다.

변경 분류: **B 기존 결함**, 승인된 BLOCKER 3.

### 4. Host deadline과 callback cleanup budget 불일치

Diff: `N/packages/framework/src/runtime/host/index.ts:963`에서 최초 shutdown만 deadline을
정한다. `route-mesh-runtime.ts:349,546`은 그 절대 시각으로 기존 deadline timer를 설정한다.
`host/index.ts:1743,1914`→`spots/index.ts:1102`→`spot-activation.ts:965`→
`spot-closing.ts:8`로 같은 deadline을 전달한다. Force-stop도 잔여 시간이 0인 callback을
기존 cleanup timer로 끝내며 30초 budget을 새로 받지 않는다.

Entry cleanup에도 `host/runtime-shutdown.ts:50`→`spot-node-runtime-manager.ts:818`→
`spot-entry-activation.ts:307`로 deadline을 전달한다. `host/index.ts:1086`은 최종 resource
cleanup까지 끝난 시각으로 `Stopped` 가능 여부를 확인한다. 새로운 timer나 budget은 없다.

소유 계층: Framework shared host operation이 deadline을 정하고 Spot closing이 정리용
signal과 callback timer를 소유한다.

Spec 조항: Host relocation flow §14 단계 6 및 마지막 문단. 남은 deadline의 cleanup signal,
기한 초과 후 bounded teardown, `ForceStopped/DeadlineExceeded`.

교차언어 대조: Java `ZLinkFrameworkRuntime.java:2127`은 저장한 `terminationDeadline`을
`continueDrain(HOST_SHUTDOWN, deadline)`으로 전달한다. Java Instance close도 절대 deadline을
받는다. .NET `ZLinkFrameworkDrainExecutor.cs:101,227`은 `absoluteDeadline`을 `DrainSpots`로
전달한다. Node의 합류 호출 갱신·coordinator 재계산·독립 callback budget을 하나로 합쳤다.

변경 분류: **B 기존 결함**, 승인된 BLOCKER 4의 deadline 소유 경로 수정.

### 5. Client-only membership에 대한 Draining weight 게시

Diff: `N/packages/framework/src/runtime/spots/spot-node-runtime-manager.ts:813`으로 기존
server channel 선택식을 모았다. Startup(`:360`), descriptor(`:513`), host weight 게시
(`host/index.ts:1779`)가 그 소유자의 `serverChannels()`를 재사용한다. 별도 channel 목록이나
client를 server로 등록하는 경로는 없다.

소유 계층: Framework MeshNode 등록 소유자.

Spec 조항: [MeshNode §2](../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md)의 ChannelName set은 Server role 목록이다. §8 및 Host relocation flow §14의 Draining 게시도 그 등록에 적용한다.

교차언어 대조: Java `ZLinkMeshNodeRuntime.java:89`는 등록한 `channelWeights()`에서 channel을
만든다. .NET `ZLinkManagedMeshNode.cs:510,535`는 등록한 channel과 node descriptor의 상태를
갱신한다. 두 언어 모두 client membership을 가짜 server로 등록하지 않는다. Node는 raw
backend 결함이 아니라 host가 전체 membership을 잘못 열거한 결함이었다.

변경 분류: **B 기존 결함**, 승인된 BLOCKER 5.

### 추가 발견: ZoneWorld runner의 중복 강제 종료

Diff: `N/samples/ZoneWorld/Runner/sample-runner.mjs:324` 뒤의 `stopNormalTopology()` 호출과
별도 전체 역할 SIGKILL 루프를 삭제했다. 의도한 fault injection과 정상 교체의 `ctx.stop()`은
각 scenario가 계속 소유한다. 남은 역할의 마지막 정리는 기존 공통 runner cleanup이 소유한다.

소유 계층: Sample process runner. Framework runtime 변경은 없다.

Spec 조항: Host relocation flow §14의 정상 shutdown과 [ZoneWorld §9.3·§11](../../../framework/doc/framework/common/sample/zoneworld/README.ko.md). G5는 routing ID gate이며 전체 역할의 강제 종료를 요구하지 않는다.

교차언어 대조: Java `samples/java/ZoneWorld/run_sample.sh:104`는 종료를 회수한 PID를
`forget_pid`와 `node_pid` 삭제로 추적 목록에서 뺀다. Node는 이미 exit 0으로 회수한
`target-after-failure`를 다시 SIGKILL 결과로 검증했다. 해당 역할의 정상 종료와 업무
시나리오 완료는 첫 실패 run의 file log에서 확인했다.

변경 분류: **B 기존 runner 결함**. 요청의 추가 sample 실패 수정 범위다.

### 추가 발견: SupportChat idle 판정의 예약 시각 사용

Diff: `N/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/conversation-idle-timer-handler.ts:9`는 기존 `spot.onTimer()`를 호출한다. Domain adapter의 기본 현재 시각을 사용해 메시지 시각과 idle·grace 판정 시각을 맞춘다. Timeout·period·grace와 client assertion은 그대로다.

소유 계층: Sample Conversation timer adapter와 domain. Framework scheduler 동작을 바꾸지 않는다.

Spec 조항: [SupportChat §7.3](../../../framework/doc/framework/common/sample/supportchat/README.ko.md)의 마지막 message 이후 idle·grace와 재개 동작. [Spot timer §1](../../../framework/doc/framework/common/spec/server/03-spot-actor/10-spot-timer.ko.md)의 nominal tick은 domain 현재 시각을 대신하지 않는다.

교차언어 대조: Java `ConversationSpot.java:169`의 `checkIdle()`은 `System.currentTimeMillis()`를 사용한다. .NET `ConversationIdleTimerHandler.cs:19`는 tick을 무시하고 `CheckIdleAsync()`를 호출하며 `ConversationSpot.cs:233`이 현재 시각을 읽는다. Node만 `tick.scheduledAt`을 domain의 `now`로 전달했다.

변경 분류: **B 기존 sample 결함**. Aggregate의 SupportChat에서 두 번째 idle 이후 닫힘 알림 대기가 timeout이었다. 기존 flow/file log를 먼저 보존·대조했다. 지연된 예약 시각을 전달하는 결정적 회귀 테스트는 수정 전 idle 전이 누락으로 실패했고, 수정 후 idle→closed와 agent 반환을 확인했다. 임시 client 실패 진단은 제거했다.

## 규칙 수와 대안

규칙 수는 이번 변경이 다루는 결정 경로를 센다. 무관한 lifecycle 규칙은 포함하지 않는다.

| 소유자·결정 | 수정 전 | 수정 후 |
|---|---|---|
| Host publication | 게시 성공 + 성공 뒤 시간 대기: **2** | 성공 terminal: **1** |
| Spot lifecycle 종료 시작 | 명시 close + membership departure 뒤 drain close: **2** | reason에 따른 기존 close operation: **1** |
| Spot manager 종료 완료 | detached task 오류 + collection-empty 완료: **2** | close terminal 전체 수집: **1** |
| Host shutdown deadline | host 합류 갱신·coordinator 재계산·callback 독립 budget: **3** | 최초 operation의 절대 deadline: **1** |
| MeshNode server membership | startup·descriptor·host의 별도 선택식: **3** | 등록 소유자의 선택식: **1** |
| ZoneWorld 최종 정리 | sample별 전체 SIGKILL + 공통 cleanup: **2** | 공통 cleanup: **1** |
| SupportChat domain 시각 | message의 현재 시각 + timer의 예약 시각: **2** | 기존 domain 현재 시각: **1** |

수정 전/후 규칙 수: 승인된 Framework 범위 **12 → 5**, 추가 sample 수정 포함 **16 → 7**.
별도 drain 상태·mesh-empty waiter·detached close를 제거했다. 새 timer·retry·poller는 0개다.

대안 비교: shutdown 전용 bypass flag와 별도 error channel을 추가하면 lifecycle 판단과
terminal 소유자가 늘어난다. 기존 close reason과 registry Promise를 재사용하면 종료 판단을
한곳에 유지할 수 있다. Channel 목록을 host에서 다시 필터링하는 대안도 등록 규칙을 중복하므로,
startup·descriptor가 사용하던 선택식을 소유자 안으로 모았다.

## 검증

환경: `N/`, `TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` unset,
`flock -w7200 /tmp/zlink-node-gate.lock`. 설치 Core SHA-256:

```text
98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb
```

| 검증 | 결과 |
|---|---|
| `npm run build` | PASS |
| `npm run typecheck` | PASS |
| 변경 TS·JS·MJS ESLint | PASS |
| `nestjs-shutdown` | **4/4**, 기존 assertion 그대로. Callback failure·deadline 모두 PASS |
| `stream-heartbeat-shutdown` | **1/1** |
| `sample-runner-teardown` | **4/4** |
| Spot manager·activation·host focused | **120/120** |
| touched subsystem 23 files | 최초 **818/819**. 아래 계약 충돌 테스트 1개 교정 뒤 해당 파일 **2/2** |
| 최종 host deadline·publication·Nest | **33/33** |
| SupportChat domain·timer·sample 계약 | **8/8**. 지연 tick 회귀는 수정 전 fail→수정 후 pass |
| 최종 `git diff --check` | 변경 범위 PASS |
| `bash samples/run_samples.sh` | 최초 1회 SupportChat idle 대기 실패. 수정 후 최종 1회 **exit 0**, 7개 모두 완료 |
| `npm test` 한 번 | **exit 1: 1,590 pass / 1 fail / 0 cancelled / 0 skipped**. announced=completed=1591 |

`test/contract/store-failure-graceful-drain.test.js:12`의 기존 테스트는 삭제 승인된 게시 후
polling interval 대기를 요구했다. §14에 따른 publication 완료→Spot callback/cleanup→owner
cleanup 순서를 검사하고, mock timer를 진행하지 않아도 종료되는지를 검사하도록 교체했다.
지정한 Nest host 4개 테스트와 heartbeat·runner 테스트의 assertion은 수정하지 않았다.

추가 회귀: `test/contract/spot-manager.test.js`의 occupied User/Instance callback membership 보존,
오류가 있어도 다른 Spot terminal까지 대기. `topology-runtime-projection.test.js`의 공유 deadline,
server/client 혼합 membership, 최종 resource cleanup deadline. 테스트 기대치는 §14에 근거한다.

| 개별 sample | Exit | Cleanup SIGKILL | 결과 |
|---|---:|---|---|
| TicTacToe.Ts | 0 | 없음 | PASS |
| Bingo.Ts | 0 | 없음 | PASS |
| DeliveryDispatch.Ts | 0 | 없음 | PASS |
| SupportChat.Ts | 0 | 없음 | PASS |
| GameQuest.Ts | 0 | 없음 | PASS. 의도한 owner fault SIGKILL은 scenario가 회수 |
| ShoppingMall.Ts | 0 | 없음 | PASS |
| ZoneWorld | 0 | 없음 | PASS. B8와 full lane의 전체 35개 verdict |

## BLOCKERS

**공유 ZoneWorld UI의 키 listener 등록 race — 범위 확대 승인 대기.**

- 원인 위치: `framework/languages/shared_sample/zoneworld/client/src/features/move-player/use-keyboard-movement.ts:4`는 `useEffect`에서 keydown listener를 등록한다. `pages/game/game-page.tsx:15`의 joined 화면은 먼저 표시될 수 있어 passive effect 전 첫 입력이 유실된다.
- 실제 실패: `npm test`의 `sample-regression.test.js:2306`은 내부적으로 전체 sample runner를 실행한다. 그 하위 shared browser `tests/live/server.spec.ts:15`에서 첫 ArrowRight 뒤 기대 `30, 25`, 실제 `25, 25`로 실패했다. 이어지는 Ops browser 테스트 2개는 통과했다. 모든 teardown에서 cleanup SIGKILL은 없었다.
- 독립 재현: 원본 hook을 그대로 bundle하고 passive effect 실행만 보류한 browser에서, 준비된 화면의 첫 ArrowRight가 callback을 호출하지 않았다. Effect를 실행한 뒤의 ArrowRight는 `[5, 0]`을 전달했다. Framework·Core·binding 없이 재현되므로 Node transport에서 보상하지 않는다.
- 검토 제안: 해당 hook의 import와 호출을 `useLayoutEffect`로 바꾸는 2줄 patch. DOM commit에 listener 설치를 맞추며 timer·retry·test 대기를 추가하지 않는다. 실제 파일을 쓰지 않는 in-memory bundle로 첫 입력 처리를 확인했다.
- 적용 상태: **미적용**. 사용자가 `framework/languages/shared_sample/**` 수정을 명시적으로 금지했다. 해당 파일만의 수정 및 최종 재검증을 요청했다. Npm 전체 gate는 재실행하지 않았다.

소유 계층: 모든 server 언어가 공유하는 browser UI의 입력 listener lifecycle.

Spec 조항: ZoneWorld §9.1의 방향키 이동 및 server-authoritative 상태 표시. UI의 입력 admission을 Node server나 runner의 대기·재시도로 보상하지 않는다.

교차언어 대조: Node·Java·.NET의 같은 shared browser source에 적용되는 UI 결함이다. 독립 재현에 server 언어가 필요하지 않는다.

변경 분류: **B 기존 shared UI 결함, 미구현·범위 승인 대기**. 적용 시 입력 준비 규칙은 화면 준비와 passive listener 준비의 **2 → 1**로 합쳐진다. 위 구현 완료 규칙 수에는 포함하지 않았다.

## 증거 위치

[전체 로그](../../../zlink-work/c016/logs/fix-node-host-shutdown-owner/):
`build-final.log`, `typecheck-final.log`, `lint*.log`, `regression.log`, `subsystem-files.txt`,
`subsystems.log`, `publication-contract.log`, `final-host-contract.log`, `individual-results.txt`,
`sample-*.log`, `aggregate-final.log`, `aggregate-final-result.txt`, `npm-test.log`, `npm-test-result.txt`. 첫 ZoneWorld 실패의 원래 role/file log는 `zoneworld-initial-evidence/logs/`에
보존했다. Aggregate SupportChat의 flow/file log는 `supportchat-aggregate-evidence/logs/`에 보존했다. `supportchat-clock-before.log`, `supportchat-clock-after.log`가 지연 tick 회귀의 전후 결과다. 임시 runtime logging은 추가하지 않았다.

Shared UI blocker 증거: `zoneworld-npm-evidence/logs/shared-browser-playwright.log`,
`shared-keyboard-repro.cjs`, `shared-keyboard-repro.log`, `shared-keyboard-owner.patch`,
`shared-keyboard-proposal.log`. Proposal은 원본 파일을 수정하지 않는 bundle 변환으로 검증했다.
Npm 집계는 `npm-test-counts.json`에 보존했다.
