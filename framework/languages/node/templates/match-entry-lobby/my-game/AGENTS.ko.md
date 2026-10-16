# MyGame 개발 가이드

## 프로젝트 개요

이 Node.js 22(TypeScript, NestJS) 프로젝트는 매칭 로비와 server-authoritative 대전 방을 가진 액션/FPS server 예제다. 플레이어는 STREAM으로 로비에 들어가 mode별로 짝지어지고, 동적으로 생성한 Room에서 이동하고 사격한다. 연결을 처리하는 Session gateway, Matchmaking Spot과 Room을 호스팅하는 Match node A·B와 위치·relocation 상태를 저장하는 Redis를 실행한다. `package.json`은 ZLink Framework와 connector package를 `0.30.1`으로 고정한다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다. 같은 내용의 영어판은 [AGENTS.md](AGENTS.md)다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [Node.js 가이드](https://docs.zlink.systems/ko/node/guide/server/), [API reference](https://docs.zlink.systems/ko/node/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 `package.json`에 고정된 package의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 runner를 모두 실행하므로 호스트에 Node.js가 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario hit,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 --check-structure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario hit,reconnect,update
./run-e2e.sh --check-structure
```

호스트 process 디버깅은 Node.js 22 이상과 npm을 설치한 뒤 `--local` 또는 `-Local`로 실행한다. node는 로컬 process로, Redis는 Docker로 실행된다. 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가한다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/session/main.ts`는 STREAM과 mesh listener를 등록하고 `server/session/session.ts`는 연결 callback과 수신 packet 전달을 담당한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. `Session`은 첫 패킷으로 `QueueRequest`만 받으며, 인증은 binding 전에 추가하고 게임 packet은 기존 typed relay로 전달하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### 매칭과 Room

Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이다. `server/match/spots.ts`의 `Matchmaking`은 플레이어를 대기열에 넣는 고정 로비 Spot이며, `server/match/domain.ts`의 `MatchmakingState`가 대기열과 room 순번을 가진다. 같은 `spots.ts`의 `Room`은 대전마다 생성되고 `domain.ts`의 `RoomState`가 위치·HP·행동 sequence·벽 교차·승자를 가진다. 게임 규칙은 `domain.ts`에서 바꾸고, Framework callback과 I/O는 `spots.ts`와 `handlers.ts`에 둔다. ID로 찾는 게임 상태 객체가 Actor이며, `server/match/player.ts`의 `Player`는 application 상태가 없고 거부된 참가를 알린다.

`server/match/main.ts`가 Entry Spot, factory, handler를 등록하고, `bootstrapMatchmaking` 설정이 true인 node에서 고정 `matchmaking` Spot을 생성한다. 플레이어별 규칙과 Room 규칙을 연결하는 handler는 `server/match/handlers.ts`에 추가하고, 새 handler는 `server/match/main.ts`의 `providers` 배열에 모두 나열한다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다. `roomCapacity` 설정은 node당 Room Spot 수를 제한한다.

### 공통 계약과 hosting

`server/shared/messages.ts`는 process 간 메시지를, `server/shared/constants.ts`는 이름·시나리오 ID·한도·제어 문자열을, `server/shared/config.ts`는 process 설정을 정의한다. `server/shared/hosting.ts`는 hosting과 진단을 설정하고 실행 중 제어 명령을 처리하며, `server/shared/diagnostics.ts`는 진단 기록을 stderr로 쓴다. 메시지는 `server/shared/messages.ts`에 정의한다. build가 이 디렉터리에서 packet schema를 생성하기 때문이다. 대전 상태는 `Matchmaking`과 `Room`에 유지하여 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### E2E client

`e2e/client.mts`는 공개 connector로 게임에 접속하여 client가 관찰할 결과를 검사하는 시나리오를 실행한다. `e2e/runner.ts`는 node를 시작하고 로그를 수집하며 실행이 만든 자원을 정리하고, `e2e/structure.ts`는 구성을 검사한다. 새 게임 동작은 client가 받은 결과로 검증하며, 별도 unit-test 프로젝트는 포함되어 있지 않다.

## 프로젝트의 ZLink 개념

### RouteMesh와 channel

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `Names.mesh`(`my-game-arena`)를 사용한다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/node/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`Entry`는 Actor의 첫 입장을 처리하고 `Matchmaking`은 고정 로비 Spot이며, `Room`은 동적으로 생성한 대전 하나를 표현한다. `Player`는 자체 게임 상태가 없으므로 Actor가 이동해도 Spot이 사실을 유지한다. [Spot 가이드](https://docs.zlink.systems/ko/node/guide/server/21-spot/)는 이 객체들의 등록과 호출을, [Actor 가이드](https://docs.zlink.systems/ko/node/guide/server/22-actor/)는 Actor의 생성과 호출을 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/node/guide/server/23-stream/)에서 `Session`은 `QueueRequest`로 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/node/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 Redis 구현을 사용한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/node/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `EnterQueue`는 `joinSpot(...).defer()`로 `Matchmaking` 참가를 지연 처리로 요청하고, `Matchmaking`은 짝마다 Room을 만들어 Actor에 `RoomAssignment`를 보낸다. 이어서 `Assignment` handler가 Room 참가를 요청하고 `Room.onActorJoin`이 `RoomState.admit`으로 수락하며, `Player.onJoinCompleted`가 거부를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/node/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter는 게임 상태(`MatchmakingRelocation`·`RoomRelocation`이 저장하는 `MatchmakingState`와 `RoomState`)를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `server/shared/hosting.ts`의 `runHost`는 `drain` 명령에서 공개 host relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/node/guide/server/37-relocation/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 패킷 `QueueRequest`를 보낸다. `Session`은 첫 패킷 이름을 확인하고 `playerId`로 Player Actor를 `getOrCreate`한 뒤 Session에 binding하고 원래 패킷을 relay한다.
2. 새 Actor는 Entry Spot `Entry`에서 시작한다. `EnterQueue` handler가 identity를 확인하고 `joinSpot(Names.matchmakingId, ...).defer()`를 요청한다. `Matchmaking.onActorJoin`이 플레이어를 대기열에 넣고 `Matchmaking.onJoinedActor`가 같은 mode의 플레이어를 짝짓는다.
3. 짝이 생기면 `Matchmaking`이 room ID를 할당하고 `Room`을 `getOrCreate`한 뒤 player ID로 두 Actor에 `RoomAssignment`를 보낸다. `Assignment` handler가 `joinSpot(roomId, ...).defer()`를 요청한다. `Room.onJoinedActor`가 `MatchFound`를 보내고 `RoomView`를 broadcast하며 `Delivery`가 이 push를 각 binding된 Session으로 전달한다. 이미 `Matchmaking`에 있는 플레이어가 `QueueRequest`를 다시 보내면 `QueueAgain`이 대기열에 넣는다.
4. `Move`와 `Shoot`은 `Room.move`와 `Room.shoot`을 호출한다. 유효한 동작은 `RoomView`를 broadcast하고, 승리하는 사격은 `MatchEnded`도 broadcast한다. 유효하지 않은 동작은 요청자의 binding된 Session으로 `ActionRejected`나 `ShotRejected`를 보낸다.
5. 재접속하면 새 Session이 `QueueRequest`를 다시 보낸다. Actor가 속한 Room의 `Reconnect` handler가 identity를 확인하고 binding된 Session으로 view를 다시 보낸다.
6. node를 교체할 때 `runHost`의 `drain` 명령이 relocation을 요청한다. Framework는 `MatchmakingRelocation`·`RoomRelocation`의 `capture`로 게임 상태를 받아 다른 node에서 `restore`한다.

## 이 샘플을 보며 개발하기

이 샘플은 기능들이 함께 동작하는 방식을 보여 주므로, 자기 게임으로 바꾸지 않고 읽고 실행하는 용도로 쓴다. 자기 프로젝트는 Node.js 기본 셋업에서 시작한다. [ZLink Start](https://zlink.systems/start)의 Initializer에서 Node.js를 고르거나 ZLink 저장소의 `framework/languages/node/templates/starter/my-game`을 사용한다. 그다음 필요한 기능을 이 샘플에서 하나씩 옮긴다. 구조 선택은 기본 셋업의 `AGENTS.md`가 설명하며, 이 절은 `match-entry-lobby`만 다룬다. 이 샘플은 바꾸지 않고 프로젝트 옆에 참고용으로 둔다.

### 샘플 먼저 실행하기

옮기기 전에 [개발 명령](#개발-명령)으로 빌드와 e2e를 한 번 실행해 받은 샘플이 동작하는지 확인하고 동작을 본다. 이 결과가 이후 비교의 기준이 된다. 실패하면 먼저 [README](README.md)의 Known issues와 대조한다.

### 옮길 기능

기능 이름은 기본 셋업 `AGENTS.md`의 기능 표와 같다. 경로는 이 디렉터리를 기준으로 한다. 기본 셋업에는 Session node 하나와 `connect` 시나리오만 있으므로, 마지막 열은 기능마다 기본 셋업에서 함께 늘릴 것이다.

| 기능            | 이 샘플의 파일                                                                                                                                                                                                 | 옮길 것                                                                                                                                                                                                                                                                               | 기본 셋업에서 함께 늘릴 것                                                                                                                                                                        |
| --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Typed 메시지    | `server/shared/messages.ts`                                                                                                                                                                                    | 사용할 기능의 class: `QueueRequest`, `RoomAdmission`, `RoomAssignment`, `MatchFound`, `MoveRequest`, `ShootRequest`, `RoomViewRequest`, `PlayerView`, `RoomView`, `ShotRejected`, `ActionRejected`, `MatchEnded`. `SessionInfoRequest`·`SessionInfo`는 client가 필요로 할 때만 옮긴다 | `server/shared/constants.ts`의 `PacketNames` 패킷 이름, `Names`의 등록 이름, `Rejection`                                                                                                          |
| 테이블 Actor    | `server/match/player.ts`                                                                                                                                                                                       | 참가 거부 처리를 가진 `Player`, `PlayerFactory`, 빈 payload를 쓰는 `PlayerRelocation`                                                                                                                                                                                                 | `main.ts`를 가진 새 `server/match/` 프로젝트와 `server/config/` 파일, Actor factory 등록. `server/shared/constants.ts`의 `NodeEntrypoints`에 `match` 항목. Actor 선언과 구조 검사                 |
| 공유 Spot       | `server/match/spots.ts`, `server/match/domain.ts`                                                                                                                                                              | 참가 수락과 broadcast를 가진 `Matchmaking`·`Room`, 대기열과 전투 규칙을 가진 `MatchmakingState`·`RoomState`                                                                                                                                                                           | `server/match/main.ts`의 Spot factory 등록과 `bootstrapMatchmaking` 생성. `zlink.structure.json`의 `spots`·`nodes` 선언과, 기본 셋업에서는 Spot 0개를 단정하는 구조 검사를 등록과 비교하도록 확장 |
| Entry·첫 입장   | `server/match/spots.ts`의 `Entry`, `server/match/handlers.ts`의 `EnterQueue`·`QueueAgain`·`Assignment`·`Reconnect`                                                                                             | `EnterQueue`, identity 확인, `Matchmaking`으로의 지연 참가, `Assignment`의 Room 인계, `Reconnect`의 상태 재전송                                                                                                                                                                       | Entry Spot 등록과 `entry` 선언. 첫 입장 시나리오와 재접속 뒤 상태를 비교하는 `reconnect` 시나리오                                                                                                 |
| Session binding | `server/session/session.ts`                                                                                                                                                                                    | 첫 패킷 검사, Actor 조회·생성, binding과 원래 typed 패킷 relay, `SessionInfo` 응답                                                                                                                                                                                                    | `EnterQueue`와 같은 첫 패킷 이름. 기본 셋업의 `session.ts`에 옮긴다                                                                                                                               |
| Host relocation | `server/match/player.ts`의 `PlayerRelocation`, `server/match/spots.ts`의 `MatchmakingRelocation`·`RoomRelocation`, `server/shared/constants.ts`의 `Control`과 `server/shared/hosting.ts`의 `runHost` 제어 loop | snapshot adapter의 `capture`·`restore`. `drain`·`shutdown`은 기본 셋업에 이미 있다                                                                                                                                                                                                    | `preserveStateWith` 등록. 서버 교체 뒤에도 연결·상태가 유지되고 대체 node가 새 Room을 받는지 판정하는 `update` 시나리오                                                                           |
| 동적 던전       | `server/match/spots.ts`의 `Matchmaking.match`, `server/match/handlers.ts`의 `Assignment`                                                                                                                       | 동적 User Spot 생성 형태. 이 샘플에서는 짝마다 `Room`을 생성한다                                                                                                                                                                                                                      | `zlink.structure.json`의 동적 `room` Spot 선언. 새 Room에서 대전을 진행하는 `hit` 시나리오                                                                                                        |

기본 셋업 표의 `동적 던전` 기능은 User Spot을 필요할 때 생성하는 방식이다. 이 샘플은 같은 방식으로 `Room`을 생성하므로 위 행은 Room을 설명한다.

### 옮기는 순서와 규칙

기능은 표의 행 하나씩 옮기고, 하나를 옮길 때마다 빌드, 구조 검사, 관련 e2e 시나리오를 실행한다. 시나리오 ID는 `server/shared/constants.ts`의 `Scenarios` 목록, `zlink.structure.json`의 `run.scenarios`, `run.commands`의 시나리오 선택 예시에 함께 추가한다. 판정은 실제로 받은 응답과 이벤트로 하고, 고정 대기나 기한 확대로 통과시키지 않는다.

메시지 contract부터 옮긴다. `messages.ts`에 class를 먼저 정의한 뒤, 컴파일 오류가 나는 handler를 하나씩 새 메시지로 옮긴다. build는 `server/shared`에서 packet schema를 생성하므로 다른 위치에 정의한 메시지는 등록되지 않는다. 첫 입장 패킷을 바꾸면 `Session`의 첫 패킷 검사, `EnterQueue`, `Reconnect`가 같은 패킷을 받도록 함께 고친다.

상태는 그 상태를 결정하는 객체에 둔다. 대기열과 room 순번은 `Matchmaking`, 위치·HP·행동 sequence·승자는 `Room`에 두고 `Entry`와 `Player`에는 두지 않는다. 상태 필드를 바꿀 때는 같은 객체의 snapshot과 `capture`·`restore`를 함께 고친다. 빠뜨린 필드는 relocation 뒤 사라지고, `update` 시나리오가 그 손실을 잡는다.

고정 `matchmaking` Spot은 `bootstrapMatchmaking`이 true인 node에서 생성한다. 그 node가 `READY`를 보고한 뒤에 다른 Match node를 시작하면, 그 node가 유일한 배치 후보인 동안 Spot이 선언된 소유 node에서 시작된다. 그 node의 대체 process는 `bootstrapMatchmaking`을 `false`로 시작한다. 재배치된 Matchmaking Spot이 이미 있기 때문이다.

Match나 Room 같은 역할 이름을 이 샘플과 다르게 정하려면 class 이름, `Names`, `server/config/`, `zlink.structure.json`을 한 번에 바꾼다. 이름 변경은 기능을 옮기는 변경과 나눠서 진행하면 실패 원인을 찾기 쉽다.

옮긴 뒤 `update`가 실패하면 snapshot에 빠진 상태가 있는지 먼저 확인한다. 그다음 [디버깅](#디버깅)의 message flow로 첫 실패 단계를 찾는다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 `package.json`에 고정한 공개 npm package를 사용하며, 이 프로젝트의 모듈 사이에는 상대 경로 import를 쓴다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. build가 packet schema를 생성하고 `server/shared/hosting.ts`가 이를 불러온다. relocation adapter가 직접 직렬화하는 대상은 application snapshot으로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

플레이어 HP 같은 게임 사실은 그것을 변경하는 객체 한 곳에서 관리한다. Spot에는 플레이어 ID를 보관하며, 이동 뒤에도 같은 Actor 객체 참조를 사용할 수 있다고 가정하지 않는다. host relocation의 target은 `onJoinedActor`를 다시 호출하지 않으므로 Spot은 나중에 전달할 Actor 참조에 의존하지 않는다. 요청한 Actor에게는 handler의 binding된 Session으로 응답하고, 다른 플레이어에게는 player ID로 Actor를 지정해 push한다. Actor identity는 Session binding이 소유한다. Node.js의 typed handler는 현재 선언한 class가 아닌 필드만 받으므로(#1731), `Delivery`는 전달하기 전에 패킷 이름 표로 class를 복원한다. push를 추가하면 그 표도 함께 맞춘다. Message Follow 시간과 이로부터 정한 `Limits.safeMs` 관찰 기한은 `server/shared/constants.ts`에 둔다.

### 비동기 실행과 lifecycle

공개 비동기 호출은 템플릿처럼 await하며, 동기 I/O나 busy loop로 handler 실행을 중단하지 않는다. `SpotWide`가 같은 상태의 실행을 제한하므로 별도 lock이나 queue로 같은 상태를 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/node/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 각각 정해진 lifecycle 단계에 둔다. 필요할 때 ID로 생성하는 `Room`에 User Spot의 계약을 적용하며, 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/node/guide/server/34-activation-lifetime/)이 설명한다. 대기열과 Room 참가는 `joinSpot(...).defer()`와 공개 완료 callback을 사용하고 host 종료는 relocation과 runtime 관찰로 확인하여 요청 수락을 완료로 오인하지 않는다.

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 Room 상태를 이어서 사용한다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/node/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `dist/`, `node_modules/`는 commit에서 제외한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `e2e/runner.ts`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다.

설정 파일은 `matchA.json`, `matchB.json`, `session.json`이다. `e2e/runner.ts`의 `NodeProcess`는 진단 stderr를 `<node>-<n>.err.log`에, 준비·제어 stdout을 `<node>-<n>.out.log`에 기록한다. `<n>`은 실행에서 시작한 process의 순번이며, 교체한 node는 더 큰 `<n>`의 두 번째 쌍을 가진다. 시나리오가 받은 client 패킷은 `<scenario>-<suffix>.jsonl`에 있다. 실행의 Docker 명령 로그는 `docker-<label>.log`이고 로컬 빌드 출력은 `.logs/build.log`에 있다. shell runner의 Docker 빌드, Redis, 바깥 정리 로그는 `.logs/docker-<project>/`에 있다.

### 진단 수준

`server/shared/hosting.ts`의 `runHost`는 메시지 수신부터 처리 완료까지의 단계를 기록하는 message flow를 다음 설정으로 활성화한다.

```typescript
builder.configureDispatch().messageFlow('normal');
```

`normal`은 receive·dispatch·completion 단계와 오류를 기록한다. 허용하는 mode는 `off`, `errors`, `normal`, `detailed`이며, `detailed`는 더 많이 기록하므로 필요한 조사 범위에서만 사용한다.

`server/shared/config.ts`는 diagnostics key를 정의하지 않으므로 시작 설정은 hosting 호출을 수정하고 다시 빌드·시작하여 적용한다. 이 프로젝트의 stdin 제어 명령은 실행 중에 mode를 바꾸지 않는다. 진단 항목과 설정은 [Monitoring](https://docs.zlink.systems/ko/node/guide/server/26-monitoring/)과 [Observability diagnostics](https://docs.zlink.systems/ko/node/reference/08-observability-diagnostics/)가 설명한다.

### Message flow 조사

`server/shared/diagnostics.ts`는 진단 기록을 한 줄의 JSON으로 stderr에 쓰므로 message flow는 node의 `.err.log`에서 확인하며 별도 flow 파일을 만들지 않는다. record의 `flow`는 node를 통과하는 메시지를 추적하는 값이고 `corr`는 request와 reply의 관계를 확인하는 값이다. `body` 필드는 `zlink flow:`로 시작하며 실패는 `outcome=failed`, `error_type`, `error_message`로 표시된다.

실패 필드와 바로 앞 단계를 함께 읽고 category를 제외하지 않는다. 실행마다 flow ID가 달라지므로 통과·실패 실행은 논리적 작업, Actor ID와 단계 순서로 비교하여 처음 누락되거나 실패한 단계를 찾는다. startup이 message flow 기록 전에 실패했으면 시작 예외와 stack을 보존한다.

### 보조 trace와 정리

로컬 process 모드의 relocation 추가 trace는 node를 시작하기 전 부모 환경에서 설정한다. 기본 Docker node는 호스트 환경 변수를 상속하지 않는다. PowerShell에서는 다음 환경 변수를 사용한다.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_RELOCATION = "1"
```

이 값은 시작하는 child process에 적용되며 stderr를 message flow와 함께 보존한다. Bash에서는 같은 변수를 다음과 같이 설정한다.

```bash
export ZLINK_DEBUG_FRAMEWORK_RELOCATION=1
```

`ZLINK_DEBUG_FRAMEWORK_RELOCATION`은 공개 `0.30.1` package에 있는 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

## ZLink 결함 보고

### 결함 판단

ZLink 결함은 공개 API를 문서대로 호출했는데 Framework가 문서와 다른 결과를 내는 경우다. 먼저 사용한 API를 위의 공개 가이드·샘플과 대조하여 application 사용 오류인지 확인한다. 사용 오류가 아니면 우회 코드를 추가하지 않고 issue로 보고하며, scenario assertion은 그대로 유지한다. 우회 코드는 결함을 감추고, 수정된 package로 올린 뒤에도 남아 동작을 바꾸기 때문이다.

### 기존 issue 확인

등록 전에 [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue)에서 오류 메시지·예외 타입·API 이름으로 열린 issue와 닫힌 issue를 함께 검색한다. 같은 결함이 있으면 새 issue를 만들지 않고 그 issue에 재현 환경과 로그를 덧붙인다. 닫힌 issue와 같은 결함이면 수정이 포함된 package 버전을 issue에서 확인하고, 이 프로젝트의 고정 버전을 올려 다시 실행한다.

### issue 본문

본문은 ZLink 저장소의 issue template 항목을 따른다. 다음 형식을 복사해 채운다.

```markdown
# Issue description

<scenario and the public operation that fails>

# Environment

- zlink version: <package versions from package.json>
- OS: <OS, Node.js version>

# Minimal test code / Steps to reproduce the issue

1. <commands or minimal code using public APIs only>

# What's the actual result? (include assertion message & call stack if applicable)

<first failing node-log excerpt, exception and stack, flow/corr records>

# What's the expected result?

<observable result the guide or reference documents>
```

재현은 이 프로젝트 전체가 아니라 실패한 공개 호출만 남긴 최소 코드나 명령으로 줄인다. 로그는 첫 실패 구간과 그 앞 단계만 붙이고, 전체 로그가 필요하면 파일로 첨부한다. startup이 flow 기록 전에 실패했으면 그 사실을 적는다.

> **issue는 공개된다.** 등록 전에 로그와 설정에서 license key, token, 내부 host 이름, 개인 정보를 지운다.

### 등록 절차

코딩 에이전트는 issue 본문을 작성해 개발자에게 보여 주고, 개발자가 확인한 뒤에 등록한다. issue는 외부 저장소에 공개되는 글이므로 개발자의 확인 없이 등록하지 않는다. GitHub CLI를 쓰면 확인한 본문 파일로 다음과 같이 등록한다.

```bash
gh issue create --repo zlink-systems/zlink --title "<area>: <observed failure>" --body-file issue.md
```

제목에는 언어와 실패한 동작을 적는다(예: `framework(node): Spot startup fails with empty SpotId`). 등록한 issue 번호는 README의 Known issues에 적어, 수정 package가 나오면 다시 검증할 위치를 남긴다.
