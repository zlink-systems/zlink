# Node sample 종료 SIGKILL 진단 — STAGE 1

2026-09-05. 감독의 구현 승인 판단을 위한 진단이다. Source·test·runner·package와 보호 문서는
수정하지 않았다. 이 문서만 추가했으며 commit하지 않았다.

## 판정

동일한 SIGKILL 문구에 서로 다른 원인이 포함된다.

| 대상 | 확인한 원인 | 소유 계층 / 분류 |
|---|---|---|
| TicTacToe Play, Bingo Matchmaking/Play, SupportChat Support, ZoneWorld ZoneNode | Nest 종료가 host `shutdown()`을 거치지 않고 `stop()`으로 transport만 정리한다. User/Instance Spot과 반복 timer가 남는데 runtime은 `Stopped`로 바뀐다. | Framework host/Nest integration / **B 기존 결함** |
| ZoneWorld Ops | Heartbeat가 Node 주 실행 thread에서 binding `submit_sync()` → Core `NONE` send를 호출한다. Admission 대기 중 SIGINT의 JS handler도 실행되지 못한다. | Framework STREAM adapter/control send / **B 기존 결함** |
| GameQuest Mission owner | Sample이 owner-loss 검증을 위해 의도적으로 SIGKILL한 process를 runner cleanup이 실패로 다시 집계한다. 나머지 역할은 종료했다. | Sample runner / **B 결함**, `0046fa5797`에서 도입된 오분류 |

TicTacToe와 Bingo의 timer 누락은 `7ffb8e55d9` 직전 Core **`a0157dc270`으로도 재현**했다.
오늘 Core close/executor 변경을 이 현상의 원인으로 볼 근거가 없다. Ops의 최초 발생 Core
revision은 별도 bisect하지 않았다. Ops의 동기 send 선택은 `360181172f3`(09-04)부터 존재한다.

기존 summary의 “GameQuest teardown hang”은 정정이 필요하다. 이번 SupportChat 실행에서는
`session`이 exit 0이었다. 이전 session SIGKILL을 timer 결함이나 Ops heartbeat 결함으로
확정하지 않는다. ZoneWorld는 B8 child cleanup에서 실패하여 full lane은 실행하지 못했다.

## 실행 환경과 바이너리 확인

모든 Node 실행은 `framework/languages/node`에서 `TMPDIR=/dev/shm/zlink-tmp-node`,
`ZLINK_LIBRARY_PATH` unset, `flock -w7200 /tmp/zlink-node-gate.lock` 조건으로 직렬 실행했다.
Node는 v22.23.2다. Sample runner가 요구하는 sample TypeScript/schema/browser build만 실행했다.
Core/binding package는 재빌드하지 않았다.

조사 시작 시 설치본과 12:54 tgz의 Core가 달랐다. 아래 값은 SHA-256이다.

| 바이너리 | hash |
|---|---|
| Framework `node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0.17.0` | `e8c86fc6314fc073bea8f75c027f57cfa1f8fe4f92a6113865f987f65bf10530` |
| `.artifacts/wsl/npm/zlink-systems-zlink-0.17.0.tgz` 안 Core | `98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb` |
| 직접 빌드한 `a0157dc270` Core | `61a270e83040f84d06e6dc12a45556100ae7ec1f0da8feef76ab09e6635b3075` |
| 설치본/tgz의 `zlink.node` — 두 파일 동일 | `4d12c69fc0aed099feecd8f6596d99dd75c5ca3b87dd2feadd065595efdce308` |

설치본 provenance는 `2e3b1b47e4`, tgz는 `619a09043d`이며 둘 다 `dirty: true`다.
따라서 provenance revision만으로 package에 포함된 개별 수정 commit을 증명할 수 없다.
기존 gate가 실행 당시 어떤 Core를 load했는지도 그 summary만으로 확정할 수 없다.
이번 본 진단은 tgz를 임시 경로에 풀어 **그 package 전체를 load**했으며, 각 역할의
`/proc/self/maps`에서 실제 libzlink 경로를 확인했다. 설치본은 바꾸지 않았다.

Node tgz에는 Core `.so`가 동봉되지만 addon에 정적으로 포함되지는 않는다.
`readelf -d zlink.node`는 `NEEDED=libzlink.so.0`, `RUNPATH=$ORIGIN`을 표시한다.
`bindings/node/binding.gyp:17`과
`bindings/node/src/zlink/runtime/native/native_load_paths.ts:25`, `:82`가 개발/동봉 addon과
library 경로를 정한다. Linux에서는 process 시작 시 `LD_LIBRARY_PATH`를 지정하면 기존
addon으로 다른 Core를 사용할 수 있었다. 새 tgz 빌드는 필요하지 않았다.

이전 Core는 기존 worktree의 branch/source를 전환하지 않고 다음과 같이 별도 snapshot으로
빌드했다. `core/LICENSE` symlink에 필요한 root `LICENSE`도 archive에 포함했다.

```bash
git archive a0157dc270 core VERSION LICENSE |
  tar -x -C /home/hep7/project/zlink-core-b/diag-node-old-core
cmake -S /home/hep7/project/zlink-core-b/diag-node-old-core/core \
  -B /home/hep7/project/zlink-core-b/diag-node-old-core/build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTS=OFF \
  -DZLINK_BUILD_TESTS=OFF -DBUILD_STATIC=OFF
cmake --build /home/hep7/project/zlink-core-b/diag-node-old-core/build \
  --target libzlink -j4
# 비교 process 시작 시:
LD_LIBRARY_PATH=/home/hep7/project/zlink-core-b/diag-node-old-core/build/lib
```

## 관찰 방법과 TicTacToe/Bingo 증거

기존 sample의 OTel file exporter와 `messageFlow('normal')`을 사용했다.
`ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`, `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`도 설정했다.
TicTacToe의 flow-play 180건, flow-api 16건과 Bingo의 matchmaking 8건, play 160건,
api 40건, session 108건을 모든 category 그대로 확인했다. 기록된 non-succeeded outcome은 0건이며,
request의 `received → admitted → dispatched → replied`가 완료된다. 예를 들어 TicTacToe 마지막
flow `01a06fca-20e8-735d-8922-f92ff628416c`, corr `24cefaeedf624b569ba3839ce96d178f`는
`replied`까지 도달한다. 이후 누락은 application dispatch가 아니라 host의 object cleanup이다.
Sample exporter는 play-a/b를 같은 `flow-play.log`에 기록하므로 역할 구분에는 PID별 trace도 사용했다.

임시 `NODE_OPTIONS=--require=…` hook으로 lifecycle 메서드의 진입·반환, SIGINT,
`process._getActiveHandles()`, `_getActiveRequests()`, `getActiveResourcesInfo()`를 기록했다.
`async_hooks`로 Timeout 생성 stack과 `hasRef()`를 수집하고, SIGINT+250ms에 Node diagnostic
report와 Inspector CPU profile을 저장했다. 진단용 snapshot timer 자체는 `unref()`했다.
Host/Spot/timer 상태는 읽기만 했으며 timeout·retry·assertion은 바꾸지 않았다.

| Core / 역할 / PID | SIGINT 뒤 Context close 반환 | host stopCore 반환 | +250ms 관찰 |
|---|---:|---:|---|
| 12:54 / TicTacToe play-b / 58068 | 21ms | 22ms | handles `[]`, requests `[]`, resources `[Timeout]`; 1000ms managed timer |
| 12:54 / Bingo matchmaking / 58471 | 27ms | 29ms | 같은 상태; 10000ms managed timer |
| 12:54 / Bingo play-a / 58561 | 30ms | 31ms | 같은 상태; 200ms managed timer |
| a0157dc270 / TicTacToe play-b / 69170 | 97ms | 99ms | `game-tick`, `disposed=false`, timer를 보유한 Spot activation 잔존 |
| a0157dc270 / Bingo matchmaking / 69688 | 96ms | 97ms | Instance `match:1-10`의 `bingo-matchmaker-idle` 잔존 |
| a0157dc270 / Bingo play-b / 69748 | 121ms | 122ms | 관전용 `observe:room-…:observer` Spot의 `bingo-draw` 잔존 |

계측 비용이 포함된 시간이므로 성능 비교용 수치는 아니다. 모든 경우 Context/host 종료는
runner의 500ms보다 먼저 끝났고 close 관련 throw/reject는 없었다. Node의 active handle 목록은
timer를 표시하지 않아 `[]`만 보면 누락을 놓친다. `async_hooks`와 active resources를 함께 봐야 한다.
이전 Core에서 timer 보유 host의 상태는 enum 값 5(`Stopped`)였다.

PID 69170 CPU profile은 214 sample 중 200개, PID 69688은 212개 중 194개가 `(idle)`이다.
JS가 native 종료 호출에 묶인 상태가 아니다. Timer stack은 다음과 같다.

```text
ZLinkManagedTimer.scheduleNext
  -> setTimeout
  -> ZLinkManagedTimer.fire
  -> completeFireCore
  -> scheduleNext
```

Timer 생성은 `packages/framework/src/runtime/spots/spot-timer.ts:344`, 재등록은 `:421`,
정상 cancel의 `clearTimeout`은 `:375`다. 문제는 cancel이 실패한 것이 아니라 timer를 소유한
Spot close가 호출되지 않는 것이다. Timer는 Core socket과 무관하게 JS event loop를 계속 유지한다.

## Framework host의 누락과 소유 계약

이 절의 `N/`은 `framework/languages/node/`를 뜻한다.

실제 종료 경로는 다음과 같다.

```text
SIGINT -> sample waitForShutdown -> Nest app.close
  -> providers.ts onModuleDestroy -> runtime.stop
  -> stopCore -> stopRuntimeParts
  -> stream / Entry Spot / mesh / channel / location / context 정리
  -> runtimeState = Stopped
  -> onApplicationShutdown -> stop 재호출(이미 state 없음)
```

- 원인 진입점: `N/packages/nestjs/src/providers.ts:344-348`와
  `N/packages/framework/src/runtime/host/index.ts:1722-1725`가 모두 `stop()`을 호출한다.
- `host/index.ts:1478-1510`은 `executionState`와 transport 참조를 먼저 비우고
  `runtime-shutdown.ts:44-59`로 하위 자원을 정리한다. 이 인자/단계에 local `spotManager`의
  User/Instance activation 종료가 없다.
- `spots/spot-node-runtime-manager.ts:817-850`은 Entry activation과 mesh transport를 닫는다.
  `DefaultZLinkSpotManager`가 보유한 User/Instance activation은 그 collection에 포함되지 않는다.
- 이미 존재하는 올바른 경로는 `host/index.ts:1062-1089`의 `runShutdown()`이다.
  `routeMeshCoordinator.shutdownHost()`가 `performMeshShutdown()`(`:1741`)을 호출하고,
  `spots/index.ts:1102-1107`의 `drainForShutdown()`이 local Spot에 `HostShutdown` close를 요청한다.
  `spots/spot-activation.ts:977-998`이 closing callback, timer dispose, executor, handler와
  native resource를 정리한다. 그 뒤 `runShutdown()`이 transport `stop()`을 호출한다.

**Framework 소유 계약:**
`framework/doc/framework/common/spec/server/languages/node/01-system-structure.ko.md:164-201`
§5는 두 Nest hook이 **Shutdown을 시작하거나 같은 terminal operation에 합류**하도록 명시한다.
`05-location-relocation/05-host-relocation-flow.ko.md:759-829` §14는 admission seal → accepted
work drain → Spot HostShutdown callback → local scope/owner/descriptor/listener/transport cleanup을
정한다. 완료 결과는 `Stopped/None`, deadline/teardown 실패는 bounded cleanup 뒤
`ForceStopped/DeadlineExceeded` 또는 `ForceStopped/TeardownFailed`다.
현재 hook은 그 operation과 결과 확정을 통째로 건너뛴다. 상태 enum만 Stopped로 바뀌는 것은
이 종료 계약을 충족하지 않는다.

**Core 소유 계약:** `core/doc/spec/core/socket/README.ko.md:605-625` §6의 close는 native
pending operation/unread completion·packet을 정리한다. `core/doc/spec/core/01-context.ko.md:47-61`
§3의 ctx_term은 모든 socket의 close를 기다릴 수 있다. Core가 Framework Spot timer를
취소해야 한다는 계약은 없다.

**Binding 소유 계약:** `bindings/doc/spec/node/README.ko.md:379`, `:780-794`는 native handle
lifecycle과 completion drain/Promise 변환을 binding에 둔다. 현재 binding은 상시 별도의
JS completion poller thread를 쓰는 구조가 아니다. `bindings/node/native/src/addon_core.cc:2257-2426`의
socket readable watcher가 **Node libuv loop의 `uv_poll_t`**를 사용한다. Runtime completion owner는
`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:469-484`에서 pending state를 종결하고
watch를 닫는다. `socket_base.ts:115-122`가 이를 수행한 뒤 native socketClose를 호출한다.
`core/context.ts:166-171` → `addon_core.cc:1458-1475`의 ctx_term은 동기 C 호출이다.
이번 TicTacToe/Bingo에서는 이 호출들이 모두 반환했다.

Framework의 `node-backend-adapter-factory.ts:142-155`는 shutdown 후 close를 호출하며,
`node-backend-adapter-support.ts:165`에는 별도의 `closeWithBusyRetry`도 존재한다. 이번 close
trace에는 오류가 없어 그 retry는 원인이 아니다. 새 completion drain, poller, Core 종료 retry를
Framework에 추가할 이유가 없다. 확인된 중복은 **public Shutdown과 Nest transport-only stop이라는
host 종료 규칙의 분리**다.

## ZoneWorld Ops의 별도 STREAM 원인

B8 lane을 별도로 추적했다. PID 90869에 runner가 SIGINT를 보낸 시각은 `1788582346988`,
SIGKILL 결정 시각은 `1788582347490`이다. Process의 JS SIGINT hook은 실행되지 않았다.
Native main-thread stack은 다음 호출에서 대기 중이었다.

```text
socket_submit_send (zlink.node)
  -> zlink_send_part_rid(flags = ZLINK_SEND_FLAGS_NONE)
  -> send_completion_submit_blocking
  -> wait_for_completion_submit_admission
  -> wait_submit_progress(timeout_ms = 896)
  -> pthread_cond_clockwait
```

`zone-b8-stacks/90869.gdb`에 모든 thread stack이 있다. 같은 run의 ZoneNode PID 90898/90949는
Context close를 각각 134/140ms에 반환했고, GDB main thread는 `uv_run → epoll_pwait`였다.
이들은 `zone-tick`/`bot-tick` timer 누락이다. Ops는 **close/ctx_term에 진입하기 전** 대기다.
위 896ms는 stack 시점의 send 대기 값이며 무한 native deadlock을 증명하는 값은 아니다.

추가한 진입 trace에서 Ops PID 95101의 마지막 미반환 호출을 확인했다.

```text
stream-session-runtime.ts:647  runLivenessCheck
  -> managed-stream.ts:160  writeControl(heartbeat ping)
  -> node-socket-backend-adapter.ts:140  STREAM send
  -> node-backend-adapter-support.ts:134  submitBindingSyncSend
  -> RuntimeSendOperation.submit_sync
```

Core `NONE`의 blocking admission은 계약상 동작이며, binding도 `submit_sync()`를 `NONE`에
정확히 연결한다(`bindings/doc/spec/node/README.ko.md:785`). Framework가 async control-plane
작업을 JS event loop에서 동기 send로 수행하는 것이 수정 대상이다. 기존 async 경로인
socket `submit()`/`submitBindingAsyncSend()`가 있으므로 완료 drain이나 DONTWAIT 재제출을
Framework에서 다시 구현할 필요가 없다. Node §5의 종료 진입과 §14의 transport teardown을
주 실행 thread의 send가 차단한다.

이 판정은 Core/binding 결함 신고가 아니다. 공개 binding API로 경계를 확인하려면
`createContext()` → `createStreamSocket(ctx)` → 연결에서 얻은 RID에
`stream.send(rid).message(payload).submit_sync()`를 admission 불가 조건에서 실행하고
JS signal/timer 진행 여부를 관찰할 수 있다. `submit()`은 다른 비동기 계약이다.
이 최소 binding repro는 이번에 별도로 작성/실행하지 않았으며, 실제 sample의 native/JS stack으로
위 호출 선택을 입증했다. Ops에 대한 이전 Core 비교나 전체 Core 원인 bisect도 수행하지 않았다.

## 언어별 종료 대조

다른 언어는 이번 작업에서 다시 실행하지 않고 해당 역할의 entry point와 runtime을 대조했다.
C++ 정상 exit는 지정된 기존 gate 결과를 근거로 한다. Java/.NET의 현재 sample pass를 새로
주장하지 않는다.

| 언어 | 같은 역할의 종료 진입과 local scope 정리 |
|---|---|
| C++ | `samples/TicTacToe/Server/Play/main.cpp:19`, `samples/Bingo/Server/Matchmaking/main.cpp:19`의 `app.run()` → `framework/src/runtime/host/app.cpp:2709-2712` signal에서 `shutdown()` → `:3734-3746` Spot close → owner/transport 정리. Node처럼 transport stop만 호출하지 않는다. |
| Java | TicTacToe `samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/PlayServerApplication.java:67`, Bingo `samples/java/Bingo/Server/Matchmaking/src/main/java/systems/zlink/samples/bingo/server/matchmaking/MatchmakingServerApplication.java:54`의 bean destroy `close`. `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:1971-2023`가 Spot/Actor close를 포함한다. `runtime/spots/ZLinkSpotRuntime.java:1739-1759`는 User/Instance activation을 닫고 timerExecutor도 종료한다. |
| .NET | `samples/TicTacToe/Server/Play/Program.cs:7`, `samples/Bingo/Server/Matchmaking/Program.cs:6`의 `RunAsync()`. `src/Zlink.Framework/Runtime/Host/ZLinkFrameworkHostRuntimeCoordinator.cs:51-87`는 maintenance Shutdown 뒤 runtime Stop을 수행한다. `ZLinkFrameworkRuntimeState.cs:192-235`가 Spot lifecycle close를 transport 정리 전에 수행하고, `Runtime/Spots/ZLinkSpotNodeRuntime.cs:670-721`이 local Spot dispose를 포함한다. |
| Node | `stopCore()`가 User/Instance Spot close를 누락하고 `shutdown()`을 호출하는 Nest hook도 없다. JS의 참조된 timer가 process exit를 직접 막는다. 다른 언어의 scope cleanup이 없는 상태를 Node timer에 `unref()`만 적용해 감추면 계약 누락은 남는다. |

STREAM heartbeat도 차이가 있다. C++ `framework/src/runtime/streams/stream_runtime.cpp:1476-1496`은
async submit operation을 사용하고, Java `runtime/streams/ZLinkStreamRuntime.java:1483-1509`는
`sendControlAsync → sendAsync`를 사용한다. .NET `Runtime/Streams/ZLinkStreamFrameWriter.cs:42-47`는
heartbeat를 `SendFlags.DontWait`로 보낸다. Node의 `writeControl()`은 위 동기 NONE 경로로 연결된다.
따라서 Node 수정 사유는 단순히 플랫폼 timer의 차이가 아니라 **종료 연결 누락과 blocking API 선택**이다.

## Sample별 검증 결과와 runner 오분류

| 실행 | 결과 | 해석 |
|---|---|---|
| 설치돼 있던 Core / TicTacToe | exit 1, play-a SIGKILL | host 종료 후 Timeout. 본 판정에는 다음 tgz 비교를 사용했다. |
| 12:54 tgz / TicTacToe | exit 1, play-b SIGKILL | game-tick 잔존 |
| 12:54 tgz / Bingo | exit 1, matchmaking + play-a SIGKILL | matchmaking/관전 Spot timer 잔존 |
| a0157dc270 / TicTacToe, Bingo | 각각 exit 1 | play-b / matchmaking+play-b; 같은 timer 결함 |
| 12:54 tgz / SupportChat | exit 1, support SIGKILL | conversation-idle timer 6개 잔존; session/api는 exit 0 |
| 12:54 tgz / GameQuest | exit 1, mission-a SIGKILL | 의도된 owner-loss kill; mission-b/api-a/api-b는 exit 0 |
| 12:54 tgz / ZoneWorld | B8 child exit 1 | ops blocking heartbeat, 두 zone-node managed timer 잔존; full lane 미실행 |
| 12:54 tgz / ZoneWorld B8 추가 stack 추적 | 두 실행 모두 exit 1 | native wait와 heartbeat JS 호출 경로 확인 |
| 12:54 tgz / DeliveryDispatch | exit 0, PASS | 모든 역할 exit 0 |
| 12:54 tgz / ShoppingMall | exit 0, PASS | 모든 역할 exit 0 |

GameQuest `samples/GameQuest.Ts/Runner/sample-runner.mjs:81-86`은 owner를 찾아
`ctx.signal(owner, 'SIGKILL')`한다. `run-sample.mjs:130-134`는 이 process에 `expectedStop=true`를
설정한다. 그런데 `:482-485`가 expectedStop을 고려하지 않고 이미 발생한 SIGKILL도 cleanup
실패에 넣는다. PID 74670에는 JS SIGINT 기록이 없고, 생존한 PID 74671/74672/74673은 실제
cleanup SIGINT를 받은 뒤 exit 0이었다.

`git show 0046fa5797 -- framework/languages/node/samples/run-sample.mjs`는 기존의
SIGINT → 500ms → SIGKILL 시간은 그대로 두고 실패 집계와 marker 보류를 추가했음을 보여 준다.
`9a0371fea7`은 그 변경을 포함하는 merge이며 직접 변경 commit은 `0046fa5797`이다.
이전 runner의 exit 0은 clean teardown 증거가 아니다. 다만 예정된 owner-loss까지 실패로
만든 것은 이 변경의 별도 회귀다. “어떤 역할이든 SIGKILL이면 실패”가 아니라 **계약상 예정된
fault injection 결과와 cleanup에서 필요해진 강제 종료**를 구분해야 한다.

ZoneWorld full lane에도 예정된 node-loss(`sample-runner.mjs:225`)와 topology 교체용
`stopNormalTopology()`(`:544-554`)의 SIGKILL이 있다. 하지만 이번 **B8 ops kill은 예정된 kill이
아니다**. `signals.jsonl`은 ops에 cleanup SIGINT 후 502ms에 cleanup SIGKILL을 기록한다.
Full lane의 예정된 kill 오분류는 source상 별도 문제이며 이번 full lane 실행 결과로 주장하지 않는다.

DeliveryDispatch가 통과하는 이유를 “STREAM/ClientServer가 없어서”로 설명하면 틀린다.
`Server/Session/session-module.ts:46`, `Server/CourierSession/courier-session-module.ts:36`에 STREAM이
있고 `Server/DispatchCenter/dispatch-center-module.ts:38-44` 등에 ClientServer가 있다.
이 sample에는 해당 managed Spot timer 등록이 없으며, worker timer는
`dispatch-worker.ts:29-35`에서 Nest lifecycle이 직접 정리한다. ShoppingMall은 HTTP +
RouteMesh object client/server 구성이며(`commerce-api-module.ts:33`,
`shoppingmall-workflow-module.ts:37-44`) managed Spot timer가 없다.
두 sample의 exit 0은 timer 누락을 드러내지 않는 구조이지 모든 local scope cleanup의 증거는 아니다.

## 수정 후보와 회귀 검증

**분류 B — host/Nest:** `providers.ts`의 destroy hook과 host의 application shutdown hook을
이미 있는 shared `shutdown()` operation에 연결하는 것이 우선 후보다. Node exact spec이
이미 요구하는 계약이므로 새 public API는 필요 없다. 대안인 `stopRuntimeParts()`에 별도
Spot cleanup loop를 덧붙이면 public Shutdown과 둘로 나뉜 종료 정책을 유지하므로 권장하지 않는다.
Timer `unref()`, sample의 강제 exit, cleanup grace 연장은 근본 수정이 아니다.

**분류 B — STREAM:** `writeControl()`과 그 adapter가 기존 async submit 계약을 사용하도록
호출 경계를 정렬하는 것이 후보다. 기존 `submitRaw`/socket `submit` 경로를 재사용하고 binding의
completion owner에 대기와 결과를 맡겨야 한다. Framework retry/두 번째 poller, timeout 증가,
signal 전용 native 종료 thread 추가는 제안하지 않는다. Error reply의 `writeRaw(DONTWAIT)`도
동일 STREAM adapter가 flags를 무시하는 경계이므로 승인된 구현 단계에서 함께 계약을 확인해야 한다.

**분류 B — runner:** 기존 process expectation의 소유자인 shared runner에서 예정된 fault injection
종료를 판정한다. Sample marker/assertion을 완화해서 통과시키는 방식은 해당하지 않는다.
의도된 SIGKILL의 예상 signal/결과를 명시적으로 검증하고 cleanup 강제 종료 실패는 유지해야 한다.

구현 승인 뒤 필요한 회귀 검증은 다음과 같다. 이번 단계에서는 test를 추가하지 않았다.

1. **실제 자식 Node process의 Nest 종료:** public Framework 등록으로 Entry/User/Instance Spot과
   반복 timer를 활성화한 뒤 SIGINT → app.close를 수행한다. `process.exit()` 없이 자연 exit 0,
   SIGKILL 없음, HostShutdown callback 완료, terminal outcome/event 확정을 검증한다. 성공 marker만
   확인하는 현재 sample/unit assertion으로는 부족하다. 두 Nest hook이 같은 operation에 합류하는
   경우와 deadline/closing callback 실패의 ForceStopped 결과도 기존 lifecycle suite에서 검증한다.
2. **STREAM heartbeat와 signal 진행:** 연결된 session의 heartbeat send가 admission을 기다리는
   조건에서 timer/signal 처리가 계속 가능해야 한다. B8 disconnect/reconnect 조건에서 종료가
   실제로 진입하고 accepted send가 terminal 처리되는 것을 확인한다. boolean mock send만으로
   검증하지 않고 실제 binding을 사용한다.
3. **runner의 종료 판정:** 예정된 GameQuest owner SIGKILL + 정상 cleanup은 pass, SIGINT에 응답하지
   않아 cleanup이 SIGKILL한 role은 fail, 예상과 다른 이른 종료는 fail을 각각 검증한다.
4. TicTacToe/Bingo focused 실행 → 관련 lifecycle/STREAM suite → 최종 7-sample gate 한 번으로
   확장한다. 현재 main에는 구현을 적용하지 않았으므로 위 실패는 남아 있다.

수정 전/후 규칙 수: **이번 단계 구현 변경 0**. 제안한 host 정렬은 종료 정책
`Shutdown / transport-only Nest stop`의 2개 경로를 기존 shared Shutdown 1개로 합치는 것이다.

## 증거 위치와 진단 도구 정리

증거 root는 [`zlink-work/c016/logs/diag-node-teardown/`](../../../zlink-work/c016/logs/diag-node-teardown/)다.
`observations.json`에 PID별 handle/timer/activation snapshot과 Context/host 반환 시간이 있다.
각 case directory에는 runner log, PID `.trace`, `.report.json`, `.cpuprofile`, 수집한 `.gdb`,
sample run의 config와 flow/file log를 보존했다. Report의 environmentVariables는 보존본에서 제거했다.

- `tictactoe-new2/58068.trace`, `bingo-new/58471.trace`: 지정한 빠른 역할의 12:54 결과.
- `tictactoe-old/69170.cpuprofile`, `bingo-old/69688.cpuprofile`: 이전 Core의 CPU profile.
- `zone-b8-stacks/90869.gdb`: Ops의 native admission wait.
- `zone-b8-stacks/90898.gdb`, `90949.gdb`: 종료 뒤 Node event loop의 timer 대기.
- `zone-b8-send/95101.trace`: 마지막 미반환 heartbeat `submit_sync`의 JS stack.
- `zone-b8-stacks/signals.jsonl`: runner의 실제 SIGINT/SIGKILL 위치와 시각.

초기 GDB attach는 Yama `ptrace_scope=1` 때문에 실패했다. 추가 stack 수집에만 임시
`LD_PRELOAD` constructor로 해당 자식 process의 `PR_SET_PTRACER`를 허용했다. System 설정은
변경하지 않았다. Runner가 기존 deadline에서 SIGKILL을 결정한 직후, 실제 kill 직전에 GDB로
stack을 수집했다. 이 관찰 시간이 추가된 run을 cleanup 성능 측정으로 사용하지 않는다.

초기 tgz 압축 해제의 상대 경로 오류로 역할 시작 전에 실패한 실행과, root LICENSE가 빠진
첫 old-Core configure 실패는 재현 결과에 포함하지 않았다. 입력 경로/파일을 바로잡은 뒤 위
비교 실행을 수행했다. 임시 JS hook과 ptracer source/library는 진단 종료 시 제거했다.
보호 문서·source·test 수정, package 재빌드, commit은 없으며 **STAGE 1에서 종료**한다.
