# Node ZoneWorld 경계 bot 관측 수정

## 결과와 범위

감독이 Node ZoneWorld의 F4 실패 원인과 수정·검증 결과를 판단하기 위한 기록이다.
감독이 진단을 B(기존 결함)로 승인한 뒤 구현했다. Commit은 수행하지 않았다.

Core, binding, 다른 언어, `shared_sample`, 보호 문서와 package 설치 상태는 수정하지
않았다. 증거는 `/tmp/zlink-node-zoneworld-f4/`에 보존한다.

## 상관관계와 원인

### Timer의 nominal tick 중복

통과 실행 `/dev/shm/zlink-tmp-node/zlink-zoneworld-LIhTmH/logs/west.flow.jsonl`에서
`actor_id=bot-ne-x`, `packet_name=BotTickMsg`, `phase=dispatched`는 55,995ms 동안
197회 dispatch됐고, 인접 dispatch 81쌍의 간격은 50ms 단위 반올림으로 0ms였다.
500ms 주기의 bot이 같은 주기 부근에서 두 번 이동했다.

원인은 `framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts:431`
(수정 전)의 due index 계산이다. `SkipLateTicks`는 monotonic elapsed를 period로 나눈
몫을 최소 1로만 제한했다. `setTimeout`은 delay를 정수 밀리초로 절삭하므로 callback이
nominal 시각보다 1ms 미만 먼저 시작할 수 있고, 그때 몫은 직전에 전달한 index와 같다.
같은 `ScheduledIndex`가 두 번 전달되고 다음 예약(`scheduleNext`)도 같은 nominal 시각을
향해 0ms delay로 잡혀 callback이 연속 실행된다. Wall clock 점프와 무관하다.

회귀 테스트 `framework/languages/node/test/contract/spot-manager.test.js:3926`는 공개
`SpotContext.addTimer()`만 사용해 첫 callback에서 monotonic 시계를 0.25ms 전진시키고
platform delay를 정수로 절삭한다. 수정 전 `ScheduledIndex=[1,1,2]`
(`timer-before.log`), 수정 후 `[1,2,3]`.

### Client의 점검 대상 가정과 판정 위치

수정 전 `framework/languages/node/samples/ZoneWorld/Client/special.ts:394`는 zone
소유자와 무관하게 `zone-node-2`를 점검 대상으로 고정하고, `:402`부터 `bot-ne-x`가
NW에서 `x>=46`에 도달한 뒤 더 작은 x로 돌아오는 왕복을 관측했다. 실패 실행
`/dev/shm/zlink-tmp-node/zlink-zoneworld-3OqqSG/logs/zone-node-1.log:24`처럼 `zone-node-1`이
NE·NW를 함께 소유하면 점검은 bot 궤적과 무관한 node에 걸리고, 관측은 "거부로 인한
방향 반전"이 아니라 특정 bot의 다음 왕복(최대 32s)이 된다. Timer 중복으로 bot이 불규칙하게
빨라지면 이 왕복 관측도 함께 흔들렸다.

또한 marker가 뒤바뀌어 있었다. 공통 ledger(§11.2)는 ZW-F3=방향 반전, ZW-F4=bot 대상
push 부재인데 Node client는 push 유발 traffic 뒤에 `ZW-F3`, 반전 관측 뒤에 `ZW-F4`를
출력했다. Runner의 무세션 push 로그 검사도 F client를 시작하기 전에 수행해 그 client의
traffic을 포함하지 않았다.

## 수정과 계약

### Framework Spot timer

`spot-timer.ts:426-447 selectScheduledIndex()` — due index를
`max(lastScheduledIndex + 1, floor(elapsed / period))`로 계산한다. "최소 1" 규칙은
`lastScheduledIndex`가 0에서 시작하므로 이 하한에 흡수된다. `DelayNextTick`과
`CatchUpBounded` 분기는 같은 `nextScheduledIndex`를 재사용하며 동작이 바뀌지 않는다.
새 상태·타이머·헬퍼는 없다.

### Sample client와 runner

`special.ts runBots()`·`verifyBotReversalOnRejection()`·`findAboutToCross()` —
dotnet `Client/Scenarios.cs F4BotReversesOnRejection/FindAboutToCross`, java
`Scenarios.java f3/aboutToCross`와 같은 규칙으로 맞췄다.

- F1 뒤 X bot 중 다음 한 걸음이 수직 경계를 넘는 bot(NW에서 `x+3>=50`, NE에서
  `x-3<50`)을 관측하고, 목적 zone의 소유자를 Ops `WatchNodesRes`에서 조회해 그 node를
  점검 상태로 둔다.
- 같은 bot이 관측한 최댓점(peak)에서 반대 방향으로 돌아오면 `ZW-F3 passed`.
  점검 해제는 이 관측의 `finally`에서 한다. 대기 30s(수정 전 60s에서 축소).
- 그 뒤 announce·OutOfRange 거부·bot 포함 ZoneStateNotify로 push 경로를 지나게 하고
  `ZW-F4 passed`.
- `Runner/sample-runner.mjs:212-218` — 무세션 push 로그 검사를 F client 종료 뒤로 옮겨
  그 traffic까지 음성 증거에 포함한다(dotnet `ZW-F4-no-push`, java 312행과 같은 순서).

`test/contract/sample-zoneworld-bots.test.js`(이전 job의 client 회귀)는 유지했다.
mock의 인접 zone 가시 범위를 `x<55`에서 서버 규칙
(`Server/ZoneNode/Domain/world.ts inBorderBand`, `zoneSplit + borderBand = 60`)으로
고쳤다. 실제 서버는 NE의 `x=55` bot을 NW client에 보여주므로 mock이 서버보다 좁았던 것이다.

### 네 줄

소유 계층: Node Framework Spot timer(`ZLinkManagedTimer.selectScheduledIndex`)가 nominal tick 선택을 소유한다. 점검 대상 선택과 F3/F4 판정은 sample client가 Ops 공개 API로 소유한다.

Spec 조항: Spot timer `10-spot-timer.ko.md` §1(`ScheduledIndex`는 감소하지 않고 `DeliveryIndex`마다 정확히 1 증가·`SkippedTicks` 정의), §2 `SkipLateTicks`("관찰 시점의 최신 due tick 하나만 전달"), §3 monotonic admission; ZoneWorld `README.ko.md` §7.3 bot 궤적, §11.2 ZW-F3(방향 반전)·ZW-F4(push 부재).

교차언어 대조: .NET `Runtime/Timers/ZLinkTimer.cs:440`과 Java `runtime/spots/ZLinkSpotTimerSchedule.java:119`는 due index를 `lastScheduledIndex + 1` 이상으로 제한한다. Node에만 이 하한이 없었다(구조적 차이가 아닌 누락). ZoneWorld client는 dotnet·java와 같이 관측한 경계 bot의 목적 zone 소유자를 Ops에서 구한다. 세 언어가 한 규칙을 구현한다.

변경 분류: B — 기존 timer 결함(Node 단독 누락)과 sample 관측·판정 결함.

수정 전/후 규칙 수: timer index 선택 2→1(“최소 1”과 “elapsed 몫” 두 규칙이 “마지막 전달 다음 tick 이상의 최신 due tick” 하나로). Client 3→1(고정 node-2·고정 bot-ne-x·x>=46 왕복 세 가정이 “관측한 경계 bot의 목적 owner 점검 뒤 같은 bot의 반전” 하나로).

## 검증

환경은 `framework/languages/node`, `TMPDIR=/dev/shm/zlink-tmp-node`,
`unset ZLINK_LIBRARY_PATH`, node gate `flock -w7200 /tmp/zlink-node-gate.lock`,
sample gate `flock -w7200 /tmp/zlink-samples-gate.lock`. rebuild9 package를 재설치하지 않았다.

| 검증 | 결과 | 증거 |
|---|---|---|
| `npm run build` / `npm run typecheck` / eslint(변경 파일) | 모두 exit 0 | 세션 로그 |
| Timer 회귀 `spot-manager.test.js:3926` | 수정 전 0/1(`ScheduledIndex=[1,1,2]`) → 수정 후 1/1 | `timer-before.log` |
| `node --test test/contract/spot-manager.test.js` | 91/91 | 세션 로그 |
| Client 회귀 `sample-zoneworld-bots.test.js` | 수정 전 1/4 → 수정 후 4/4 | `regression-before.log` |
| ZoneWorld ×3 | 3/3 exit 0, 매회 `verdict ZW-*=passed` 35개·failed 0, F1·F3·F4 marker 순서 일치 | `zoneworld-run{1,2,3}.log` |
| `bash samples/run_samples.sh` ×1 | exit 0, 7 sample 모두 completed(ZoneWorld 4번째 실행 포함) | `samples-all.log` |
| `npm test` ×1 | exit 0, `ok` 1604 / `not ok` 0; `spot-manager.test.js`·`sample-zoneworld-bots.test.js` 포함 | `npm-test.log` |

## BLOCKERS

- 없음. 모든 gate가 통과했고 미해결 실패는 없다.
- 참고: 작업 tree에는 이 job과 무관한 다른 job의 미커밋 변경(dotnet `Runtime/**`·java `ZLinkJavaRawMeshNode*`,
  `bindings/node/provenance/*.json`, `bindings/node/**/completion_order.test.*`)이 함께 있다. 이 job의 변경은
  `framework/languages/node/` 아래 4개 수정 파일과 미추적 `test/contract/sample-zoneworld-bots.test.js`, 이 문서다.
