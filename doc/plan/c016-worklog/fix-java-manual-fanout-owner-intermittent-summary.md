# fix-java-manual-fanout-owner-intermittent — 요약

대상: `ZLinkManualFanoutRuntimeOwnerTest.connectionIsNotReceivableBeforeConnectCommit`
(`framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkManualFanoutRuntimeOwnerTest.java`)
`Fixture.awaitInfrastructureIdle:205` 1 s `TimeoutException` 간헐(단독 ×6 중 1, 전체 gate 1회).
Core d8b65141a4, branch main. 커밋하지 않음.

## 1. 실패 시퀀스

증거 시간표(이전 job `ui2/repro`): 통과 run은 test 전체가 0.005–0.039 s, 실패 run은 정확히 1.03 s·1.043 s —
"1 s 대기 + 정상 30 ms". 즉 느린 것이 없고, 1 s 동안 infrastructure executor의 noop이 **한 번도 실행되지
못했으며** `releaseConnect.countDown()` 직후 모든 것이 정상 진행됐다. infrastructure thread가 잡혀 있던
곳은 fixture의 `ControlledSubscriber.connect`(latch 대기)이다.

```
lifecycle thread (public connect)                   scheduler tick → infrastructure thread
────────────────────────────────                    ─────────────────────────────────────
connectEndpoint: lane turn ①  desired += endpoint
                 (join 복귀 대기 — CPU 부하로 지연)
                                                    signalTick → admittedTick 확보
                                                    runAdmittedTick → reconcileDesired:
                                                      lane turn: desired 스냅샷(endpoint 포함)
                                                      open(channel, endpoint, token):
                                                        lane turn: openingOperations.put(id)  ← 선점
                                                        openAdmitted → subscriber.connect(endpoint)
                                                          = fixture latch에서 블로킹 (infra thread)
connectEndpoint: open → lane turn ②
   openingOperations.containsKey(id) → null → 즉시 반환
   (public connect()는 socket connect 없이 반환)
test: connectEntered ✓, tick +2 ✓ (admittedTick != null이라 signalTick은 no-op이지만 wrapper tickCount는 증가)
test: awaitInfrastructureIdle → infra thread는 connect 안 → 1 s TimeoutException
finally releaseConnect → infra thread 진행 → commit → tick 종료 → close 정상(30 ms)
```

재현 run의 stall 시점 thread dump(`jcmd Thread.dump_to_file`, 임시 진단·제거함)는 §5 참고.

## 2. 원인 (file:line, 수정 전 기준)

`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkManualFanoutRuntime.java`

- `connectEndpoint` `:140-150` — endpoint를 `desired`에 등록하는 lane turn(`:141-146`)과 그 endpoint의 open을
  claim하는 lane turn(`open` `:175-184`)이 **분리**돼 있다. 두 turn 사이에 reconcile tick(`reconcileDesired`
  `:355-371` → `open(…, token)`)이 `desired`를 읽고 같은 id의 opening을 먼저 claim할 수 있다.
- 결과 ①: 등록한 caller의 `connect()`가 socket connect를 발행하지 않고 반환한다(`open` `:185` `opening == null`).
- 결과 ②: socket connect가 admitted tick 안에서 **infrastructure executor**(receive pump, 운영 배선에서는
  `ZLinkChannelRuntime:737-742`의 공유 `infrastructureExecutor`) 위에서 실행된다. fixture는 connect를 latch로
  막아 "connect commit 전 수신 불가"를 관측하므로 pump가 1 s 이상 멈춘 것으로 보인다.
- 이 형태는 spec 06 §3이 금지하는 "상태 스냅샷을 turn 밖으로 들고 나가 아직 맞다고 가정"(desired 등록 결과를
  다음 turn의 claim 근거로 사용)에 해당한다. 경합은 c67b1d8fe2 이전 코드(`connectEndpoint` → `return running` →
  `open`)에도 같은 두-turn 구조로 존재했고, c67b1d8fe2가 추가한 test가 이를 관측하게 됐다.

계약 "connect commit 전에는 수신 불가" 자체는 위반되지 않았다(`receiveAvailable`·`expireConnections`·
`publisherSnapshots`는 모두 `RECEIVABLE` phase만 본다). 깨진 것은 **open 소유자 규칙**이다.

## 3. 수정 (owner)

`ZLinkManualFanoutRuntime.java`

- `connectEndpoint`: `desired` 등록과 opening claim을 **한 lane turn**에서 수행한다(`claimOpeningInLane(id)`).
  등록한 caller가 그 endpoint의 첫 open을 소유한다.
- `open`(tick·start 경로)은 같은 `claimOpeningInLane`을 자기 turn에서 호출한다 — tick은 connection도 opening도
  없는 endpoint(닫힌 뒤 재개)만 claim한다.
- claim 조건(`running && !connections.containsKey && !openingOperations.containsKey`)은 기존 `open` 안의 inline
  코드를 `claimOpeningInLane` 한 곳으로 **옮긴** 것이며(두 호출자 공유), `openClaimed`는 기존 `open`의
  try/finally(opening 제거·complete)를 옮긴 것이다. 새 상태·타이머·옵션·분기 없음.
- Location runtime(`ZLinkFanoutLocationRuntime`)은 public connect가 없고 open이 tick 전용이라 같은 경합이 없다.

### 네 줄

- **소유 계층**: Framework `ZLinkManualFanoutRuntime`(manual fanout subscriber connection의 desired·open·phase
  owner). Core·binding 결정 아님.
- **spec 조항**: `01-execution/06-state-ownership-and-lanes` §3(상태를 읽는 코드와 결정을 확정하는 코드는 같은
  turn) · §5 "반환 전 완료 보장"(원본 동기 `connect(endpoint)`가 반환 전에 끝내던 ownership claim은 lane 이후에도
  반환 전에 끝나야 함); `02-channel-transport/01-channel-topology` §12(manual subscriber는 등록한 endpoint마다
  전용 SUB socket 하나) — 수신 가능 조건은 `05-transport-liveness`(변경 없음).
- **교차언어 대조**: dotnet `ZLinkChannelRuntimeBundle.ConnectManualCore`는 caller의 lane turn 안에서 등록과
  connect를 함께 수행; cpp `raw_fanout_subscriber_t::connect_manual`은 mutex 아래 등록+connect 동기; node
  `requestManualFanoutConnect`는 connect 요청의 lifecycle transition이 open을 소유하고 경쟁하는 주기 reconcile이
  없다. 세 언어 모두 "connect 요청자가 open 소유". Java만 주기 reconcile tick이 첫 open을 가로챌 수 있었다 —
  구조적 차이가 아니라 Java 고유 결함.
- **변경 분류**: **B(기존 결함)**.

### 규칙 수

수정 전 3(desired 등록 turn / caller의 open claim turn / tick의 open claim — 두 claimant가 임의 승자) →
수정 후 2(등록+claim 한 turn: 등록자가 첫 open 소유 / tick은 connection·opening 없는 것만 claim).

## 4. 회귀 test

같은 test에 소유자 assertion 추가: `ControlledSubscriber.connect`가 실행된 thread == `connect()`를 호출한
lifecycle thread. 수정 후 결정적으로 성립(tick이 caller의 open을 가져갈 수 있는 turn 경계가 없다). 수정 전
코드에는 같은 경합 창을 강제로 여는 seam(lane executor 주입·turn 사이 hook)이 없어 결정적 사전 실패는 만들 수
없고, 부하 ×N에서 확률적으로 잡힌다(§5 표의 pre-fix 행). seam을 위해 test hook·helper 계층을 추가하는 것은
§3 금지 패턴이라 하지 않았다. 임시 진단(`jcmd` dump-on-timeout)은 조사 후 제거했다.

## 5. 결과

EVID = `/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/mf`. 부하 = 병렬 CPU busy-loop
12개(class 반복) / 16개(probe). 환경: `TMPDIR=/dev/shm/zlink-tmp-java`, `ZLINK_LIBRARY_PATH` unset, `flock /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon`.

| 단계 | 명령 | 결과 | 증거 |
|---|---|---|---|
| 재현 A (수정 전) | class 단독 ×20, 부하 12, dump-on-timeout 임시 진단 | 20/20 PASS — 1 s 창은 20회로 재현 안 됨 | `EVID/r1.status`, `r1-*-xml/` |
| 재현 B (수정 전) | 임시 probe: 같은 fixture 500회/run, "socket connect thread == connect() caller thread" 검사, 부하 16, ×3 | **tick 소유 connect 7/500, 10/500, 3/500** (3/3 run 실패). 위반 시 `connectDone=true`(public connect가 socket connect 없이 반환), stack = `ControlledSubscriber.connect ← ZLinkManualFanoutRuntime.openAdmitted:247 ← open:187 ← reconcileDesired:364-366 ← runAdmittedTick:334 ← signalTick:321`, thread = fixture infrastructure executor(`pool-N-thread-1`) | `EVID/evidence-prefix-rep-1-xml/`, `rep-{1,2,3}-xml/` |
| 재현 B (수정 후) | 같은 probe ×3 | **0/500 ×3** | `EVID/rep2-*-xml/` |
| class ×20 (수정 후, 회귀 assertion 포함) | `:zlink-framework-core:test --tests …ZLinkManualFanoutRuntimeOwnerTest --rerun` ×20, 부하 12 | **20/20 PASS**, 3 test 모두 ≤0.06 s | `EVID/cls.status`, `cls-*-xml/` |
| 전체 gate 1 | `:zlink-framework-core:test :zlink-framework-core:contractTest --continue --rerun` | core **1259/1259**(failures 0, errors 0), contract **27/27**, BUILD SUCCESSFUL | `EVID/gate-1.log`, `gate-1-xml/` |
| 전체 gate 2 (1차) | 같은 명령 | 무효 — `--rerun`은 바로 앞 task(contractTest)에만 붙어 `:test`가 UP-TO-DATE로 재실행되지 않음(xml timestamp가 gate 1과 동일). 표에서 집계하지 않음 | `EVID/gate-2.log` |
| 전체 gate 2 (재실행) | `:zlink-framework-core:test --rerun :zlink-framework-core:contractTest --rerun --continue` | core **1259/1259**(0/0), contract **27/27**, BUILD SUCCESSFUL 44 s, xml timestamp 갱신 확인 | `EVID/gate-2b.log`, `gate-2b-xml/` |
| `git diff --check` | | clean | — |

재현 A가 0/20인 이유: 1 s 관측 창은 tick 소유 connect가 일어난 run에서만 실패하고 그 확률이 run당 ~1–2 %라서 20회로는
부족했다. probe는 run당 500 시도로 같은 사건을 직접(thread 소유자) 관측해 1 s timeout이 그 사건의 증상임을 고정했다.

변경 파일:
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkManualFanoutRuntime.java`
  (`connectEndpoint`, `open`, `claimOpeningInLane`, `openClaimed`; +30/−15)
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkManualFanoutRuntimeOwnerTest.java`
  (connect 소유 thread assertion, `ControlledSubscriber.connectThread`)
- 이 문서. core/**, bindings/**, spec, 다른 언어 변경 없음. 커밋하지 않음.

## 6. BLOCKERS / 별도 관찰

- **BLOCKER 없음.** Core·binding 원인 없음(controlled backend fixture, binding 미사용).
- 전체 gate 명령 주의: `… :test contractTest --continue --rerun` 형태에서는 `--rerun`이 `contractTest`에만 적용된다. 소스 변경
  없는 2회차는 `:test`가 UP-TO-DATE로 건너뛰므로 task마다 `--rerun`을 붙이거나 `--rerun-tasks`를 써야 한다(이 job의 gate 2
  1차가 그렇게 무효였고 재실행했다). 이전 job들의 "gate ×2" 중 2회차가 실제 재실행이었는지는 각 로그의
  `Task :zlink-framework-core:test` 줄로 확인할 필요가 있다.
- 관찰(변경 없음): tick 경로의 재개(`reconcileDesired` → `open(…, token)`)는 설계대로 admitted tick 안에서 socket connect를
  수행한다. 운영 배선에서 이 executor는 `ZLinkChannelRuntime`의 공유 `infrastructureExecutor`이고 binding connect는 비블로킹
  이라 문제가 아니지만, connect가 블로킹되는 backend에서는 pump가 그 시간만큼 멈춘다. 첫 open은 이번 수정으로 caller 소유가
  됐다.
- 관찰(변경 없음): `ZLinkSocketMonitorDrainLoop` virtual thread는 `monitor.recv()`가 반환/예외로 끝날 때만 종료된다. fixture
  Monitor는 `close()`가 `recv()`를 깨우지 않아 test당 thread 하나가 남는다(운영 binding monitor는 close 시 recv가 끝남). test
  fixture 범위의 누수이며 이번 실패와 무관.
- 작업 tree에 이 job 외의 미커밋 변경이 있다(`bindings/node/provenance/core-package-provenance.json` 수정, `bindings/node/**/
  completion_order.test.*`, `zlink-work/`, `opah/` untracked). 건드리지 않았다.
