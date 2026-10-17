# MyGame 개발 가이드

## 프로젝트 개요

이 C#/.NET 8 프로젝트는 플레이어가 연결을 유지한 채 고정 field를 넘나들고, field 경계 너머의 가까운 플레이어를 보며, 파티를 만들어 전용 Dungeon에 입장하는 MORPG server 예제다. 연결을 처리하는 Session gateway, Field host A·B, Dungeon을 담당하는 Instance host A·B와 위치·이동 상태를 저장하는 Redis를 실행한다. `Directory.Packages.props`는 ZLink Framework와 connector package를 `0.30.1`으로 고정한다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [.NET 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/), [API reference](https://docs.zlink.systems/ko/dotnet/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 이 프로젝트에 고정된 package의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 client를 모두 실행하므로 호스트 SDK가 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario field,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario field,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

로컬 process 디버깅은 .NET 8 SDK를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 Debug 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가한다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/MyGame.Session/Program.cs`는 연결 처리를 등록하고 `server/MyGame.Session/PlayerSession.cs`는 연결 callback과 수신 packet 전달을 담당한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. 첫 패킷은 `JoinWorldRequest`여야 하며 예제는 그 안의 player ID를 그대로 신뢰한다. 인증은 binding 전에 추가하고 게임 packet은 기존 typed relay로 전달하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### Field와 플레이어

ID로 찾는 게임 상태 객체가 Actor이며, `server/MyGame.Shared/PlayerActor.cs`의 `PlayerActor`가 플레이어의 좌표 `X`와 이동 `Sequence`를 소유한다. Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이며, `server/MyGame.Field/FieldSpot.cs`의 `FieldSpot`이 field 하나의 파티와 플레이어의 가시성 projection을 관리한다. 이 projection은 Actor의 이동 상태에서 복사한 관찰용 값이므로, 플레이어 상태 변경은 Actor에 적용한다.

`server/MyGame.Field/Program.cs`가 타입, handler, `border` channel을 등록하고 host가 시작될 때 node 설정의 `FieldIds`에 적힌 고정 field를 만든다. `server/MyGame.Field/Handlers.cs`는 이동, 파티 생성·참가, 던전 입장, 경계 timer·구독 handler와 push를 client로 전달하는 delivery handler 같은 Field handler를 담고 있다. `server/MyGame.Shared/WorldEntry.cs`는 첫 패킷을 받아 Actor를 좌표가 속한 field로 보내며, Field와 Instance node에 모두 등록된다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### Dungeon

`server/MyGame.Instance/Program.cs`는 Dungeon factory를 등록하고, `server/MyGame.Instance/DungeonSpot.cs`는 Dungeon 하나의 파티 명단과 stage를 관리하며 명단에 있는 구성원만 받아들이고 typed 준비 요청에 응답한다. `server/MyGame.Instance/Handlers.cs`는 `Advance`와 delivery handler 같은 Dungeon handler를 담고 있다. `DungeonSpot`은 application이 생성하는 User Spot이며 Framework가 별도 생성·초기화 계약으로 제공하는 Instance Spot과 구분된다. 전투·보상·Dungeon lifecycle은 이 프로젝트에 추가한다.

### 공통 계약과 hosting

`server/MyGame.Shared/Contracts.cs`는 field 기하와 `GameTiming`을 포함해 process 간 메시지와 게임 상수를, `server/MyGame.Shared/NodeSettings.cs`는 `FieldIds`, `FieldCapacity`, `DungeonCapacity`를 포함한 process 설정을 정의한다. `server/MyGame.Shared/GameHosting.cs`는 hosting과 진단을 설정하고 `server/MyGame.Shared/NodeControl.cs`는 실행 중 제어 명령을 처리한다. 여러 process가 사용하는 계약은 여기에 추가하며, 플레이어 상태는 `PlayerActor`에 유지하여 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### E2E client

`e2e/MyGame.E2E/GameClient.cs`는 공개 connector로 게임에 접속하고 `e2e/MyGame.E2E/Scenarios.cs`는 client가 관찰할 결과를 검사한다. `e2e/MyGame.E2E/WorldRun.cs`는 실행별 process와 기반 시설을 관리하고 `e2e/MyGame.E2E/StructureCheck.cs`는 구성을 검사한다. 새 게임 동작은 client가 받은 결과로 검증하며, 별도 unit-test 프로젝트와 formatter 설정은 포함되어 있지 않다.

## 프로젝트의 ZLink 개념

### RouteMesh와 channel

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `GameNames.WorldMesh`를 사용한다. 같은 목적의 메시지를 이름으로 구분하는 channel 중 `border`는 인접한 field 사이의 typed publication에 사용된다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/dotnet/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`WorldEntry`는 첫 패킷을 받고, `FieldSpot`은 host 시작 시 field마다 만드는 고정 User Spot이며, `DungeonSpot`은 파티마다 요청에 따라 생성되는 User Spot이다. [Spot 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/21-spot/)는 `FieldSpot.EnterAsync`가 Dungeon 준비를 위해 보내는 request를 포함해 이 객체들의 등록과 호출을 설명한다. `PlayerActor`는 소속 Spot이 바뀌어도 좌표를 유지하며, 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/dotnet/guide/server/23-stream/)에서 `PlayerSession`은 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/dotnet/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 Redis 구현을 사용한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `PlayerActor.Move`는 좌표가 속한 field에 `JoinSpot(...).Defer()`를 요청하고, `FieldSpot.OnActorJoinAsync`는 자기 field에 속한 도착만 수락하며, `PlayerActor.OnJoinCompletedAsync`는 완료 결과를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/dotnet/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter는 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `PlayerRelocation`, `FieldRelocation`, `DungeonRelocation`은 플레이어·field·Dungeon 상태를 직렬화하고 `NodeControl`은 공개 host relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/37-relocation/)가 설명한다.

### 경계 알림과 timer

주기적으로 handler를 실행하는 timer를 사용하여 `BorderTick`이 경계 근처 플레이어 정보를 publish한다. 이 pub/sub 방식에서는 `BorderHandler`가 구독한 정보를 받고, `BorderDelivery`가 수신 Actor의 binding된 Session으로 알림을 전달한다. 주기 작업의 등록과 실행은 [Timers and workers](https://docs.zlink.systems/ko/dotnet/guide/server/36-timer-worker/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 패킷 `JoinWorldRequest`를 보낸다. `PlayerSession`은 첫 패킷 타입을 확인하고 `playerId`로 Player Actor를 `GetOrCreate`한 뒤 Session에 binding하고 원래 패킷을 relay한다.
2. 새 Actor는 Entry Spot `WorldEntry`에서 시작한다. `EnterWorldHandler`가 identity를 확인하고 `PlayerActor.Move`로 좌표가 속한 field에 `JoinSpot(...).Defer()`를 요청한다.
3. `FieldSpot.OnActorJoinAsync`가 자기 field에 속한 도착을 수락하고, `OnJoinedActorAsync`가 이를 Actor에 적용하며 가시성 복사본을 기록한다. 이어서 `PlayerActor.OnJoinCompletedAsync`가 binding된 Session으로 `WorldState`를 보낸다.
4. `MoveRequest`는 `MoveHandler`가 받으며 Actor보다 1 큰 sequence를 요구한다. 같은 field 안의 이동은 Actor를 갱신하고 `WorldState`로 응답하며, 다른 field로의 이동은 `PlayerActor.Move`를 다시 호출한다. 그 field가 다른 node에 있어도 client 연결은 바뀌지 않는다.
5. timer handler `BorderTick`이 경계 근처 플레이어의 `BorderPresence`를 `border` channel에 publish하고, 인접 field의 `BorderHandler`가 가까운 관찰자에게 player ID로 `BorderVisible`을 보낸다. `BorderDelivery`가 각 관찰자의 binding된 Session으로 전달한다.
6. `CreatePartyRequest`와 `JoinPartyRequest`는 `CreateParty`와 `JoinParty`가 받아 `FieldSpot.PartyAsync`를 호출하며, 잘못된 요청은 요청자의 binding된 Session으로 `ActionRejected`를 보내 거부한다. 파티장의 `EnterDungeonRequest`는 `EnterDungeon`이 받아 `FieldSpot.EnterAsync`를 호출한다. 파티의 Dungeon을 `GetOrCreate`하고, 명단을 확정하는 typed `PrepareDungeon` request를 보낸 뒤, 각 구성원에게 player ID로 `JoinDungeon`을 보낸다. 이어서 `DungeonTravel`이 Dungeon 참가를 요청한다.
7. `AdvanceRequest`는 `Advance`가 받아 Dungeon stage를 올리고 모든 구성원에게 `DungeonView`를 broadcast한다.
8. 같은 플레이어가 재접속하면 `PlayerSession`이 기존 Actor를 새 Session에 binding하고, Actor를 가진 Spot의 `ReconnectHandler` 또는 `DungeonReconnect`가 identity를 확인한 뒤 현재 상태를 다시 보낸다.
9. node를 교체할 때 `NodeControl`의 `drain`이 relocation을 요청한다. Framework는 relocation adapter로 상태를 받아 다른 node에서 복원한다.

## 이 샘플을 보며 개발하기

이 샘플은 기능들이 함께 동작하는 방식을 보여 주므로, 자기 게임으로 바꾸지 않고 읽고 실행하는 용도로 쓴다. 자기 프로젝트는 .NET 기본 셋업에서 시작한다. [ZLink Start](https://zlink.systems/start)의 Initializer에서 .NET을 고르거나 ZLink 저장소의 `framework/languages/dotnet/templates/starter/my-game`을 사용한다. 그다음 필요한 기능을 이 샘플에서 하나씩 옮긴다. 구조 선택은 기본 셋업의 `AGENTS.md`가 설명하며, 이 절은 `morpg-fields-instances`만 다룬다. 이 샘플은 바꾸지 않고 프로젝트 옆에 참고용으로 둔다.

### 샘플 먼저 실행하기

옮기기 전에 [개발 명령](#개발-명령)으로 빌드와 e2e를 한 번 실행해 받은 샘플이 동작하는지 확인하고 동작을 본다. 이 결과가 이후 비교의 기준이 된다. 실패하면 먼저 [README](README.md)의 Known issues와 대조한다.

### 옮길 기능

기능 이름은 기본 셋업 `AGENTS.md`의 기능 표와 같다. 경로는 이 디렉터리를 기준으로 한다. 기본 셋업에는 Session node 하나와 `connect` 시나리오만 있으므로, 마지막 열은 기능마다 기본 셋업에서 함께 늘릴 것이다.

| 기능 | 이 샘플의 파일 | 옮길 것 | 기본 셋업에서 함께 늘릴 것 |
| --- | --- | --- | --- |
| Typed 메시지 | `server/MyGame.Shared/Contracts.cs` | 사용할 기능의 record: `JoinWorldRequest`, `MoveRequest`, `EnterField`, `WorldState`, `BorderPresence`, `BorderVisible`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `AdvanceRequest`, `ViewRequest`, `PartyView`, `FieldView`, `DungeonView`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `ActionRejected`. `SessionInfoRequest`·`SessionInfo`는 client가 필요로 할 때만 옮긴다 | `GameNames`의 등록 이름과 field helper, `GameTiming`, `Rejection` 사유. `NodeReady`와 `NodeProtocol`은 기본 셋업에 이미 있다 |
| Spot | `server/MyGame.Field/FieldSpot.cs`, `server/MyGame.Instance/DungeonSpot.cs` | 가시성 projection·도착 정보·파티를 가진 `FieldSpot`, 명단과 stage를 가진 `DungeonSpot` | 새 `server/MyGame.Field/`·`server/MyGame.Instance/` 프로젝트와 `server/config/` 파일, 각 `Program.cs`의 `AddSpotFactory<FieldSpot>`·`AddSpotFactory<DungeonSpot>` 등록. `FieldIds`·`FieldCapacity` 설정과 고정 field를 만드는 시작 callback. `zlink.structure.json`의 `spots`·`nodes` 선언과 이를 등록과 비교하는 구조 검사 |
| Actor | `server/MyGame.Shared/PlayerActor.cs` | 좌표 `X`, `Sequence`, `Move`를 가진 `PlayerActor`와 `PlayerFactory` | 각 `Program.cs`의 `AddActorFactory<PlayerActor, PlayerFactory>` 등록 |
| Entry·첫 입장 | `server/MyGame.Shared/WorldEntry.cs`, `server/MyGame.Field/Handlers.cs`의 `ReconnectHandler`, `server/MyGame.Instance/Handlers.cs`의 `DungeonReconnect` | `EnterWorldHandler`와 field로의 지연 join, 재접속 handler의 identity 확인과 상태 재전송 | Field와 Instance node의 Entry Spot 등록과 `entry` 선언. 첫 입장을 확인하는 `field` 시나리오와 재접속 뒤 상태를 비교하는 `reconnect` 시나리오 |
| Session binding | `server/MyGame.Session/PlayerSession.cs` | 첫 패킷 검사, Actor 조회·생성, binding과 원래 typed 패킷 relay | `EnterWorldHandler`와 같은 첫 패킷 타입. 기본 셋업의 `PlayerSession.cs`에 옮긴다 |
| Host relocation | `server/MyGame.Shared/PlayerActor.cs`의 `PlayerRelocation`, `server/MyGame.Field/FieldSpot.cs`의 `FieldRelocation`, `server/MyGame.Instance/DungeonSpot.cs`의 `DungeonRelocation`, `server/MyGame.Shared/NodeControl.cs` | `Capture`·`Restore`를 가진 snapshot record와 `drain`·`shutdown` 제어 | `PreserveStateWith` relocation 등록. 서버 교체 뒤에도 연결·상태가 유지되고 대체 node가 새 Dungeon을 받는지 판정하는 `update` 시나리오 |
| 동적 던전 | `server/MyGame.Field/FieldSpot.cs`의 `EnterAsync`, `server/MyGame.Field/Handlers.cs`의 `DungeonTravel`, `server/MyGame.Instance/DungeonSpot.cs`, `server/MyGame.Instance/Program.cs` | 파티 Dungeon의 `GetOrCreate`와 typed 준비, 명단에 있는 구성원만 받는 admission, stage | 각 Instance의 동적 `dungeon` Spot과 `zlink.structure.json`의 `GetOrCreate` edge. `dungeon` 시나리오 |
| Channel pub/sub·심리스 이동 | `server/MyGame.Field/FieldSpot.cs`의 `Configure()`·`OnInitializeAsync`, `server/MyGame.Field/Handlers.cs`의 `BorderTick`·`BorderHandler`·`BorderDelivery` | 경계 timer, 구독·발행과 수신 Actor의 binding된 Session으로의 전달 | `Program.cs`의 `border` channel 등록과 `zlink.structure.json`의 `channels` 선언. `BorderVisible` 이벤트와 `SessionInfo` 유지를 확인하는 `field` 시나리오 |

### 옮기는 순서와 규칙

기능은 표의 행 하나씩 옮기고, 하나를 옮길 때마다 빌드, 구조 검사, 관련 e2e 시나리오를 실행한다. 시나리오 ID는 `e2e/MyGame.E2E/Scenarios.cs`의 목록, `zlink.structure.json`의 `run.scenarios`, `run.commands`의 시나리오 선택 예시에 함께 추가한다. 판정은 실제로 받은 응답과 이벤트로 하고, 고정 대기나 기한 확대로 통과시키지 않는다.

메시지 contract부터 옮긴다. `Contracts.cs`에 typed record를 먼저 정의한 뒤, 컴파일 오류가 나는 handler를 하나씩 새 메시지로 옮긴다. 첫 입장 패킷을 바꾸면 `PlayerSession`의 첫 패킷 검사, `EnterWorldHandler`, 재접속 handler가 같은 타입을 받도록 함께 고친다.

상태는 그 상태를 결정하는 객체에 둔다. 플레이어별 상태는 `PlayerActor`, 여러 플레이어가 공유하는 상태는 Spot에 둔다. Session binding이 Actor identity를 소유하므로 게임 상태 view에는 Actor generation이나 Session identity를 넣지 않는다. 상태 필드를 바꿀 때는 같은 객체의 snapshot record와 `Capture`·`Restore`를 함께 고친다. 빠뜨린 필드는 relocation 뒤 사라지고, `update` 시나리오가 그 손실을 잡는다.

channel은 등록·structure 선언·구독을 같은 변경에서 함께 추가한다. 구독은 `FieldSpot.Configure()`, channel 등록은 `Program.cs`, 선언은 `zlink.structure.json`의 `channels`다. 셋이 다르면 구조 검사가 실행 전에 실패한다.

Actor를 호스팅할 수 있는 모든 node에 공통 Entry와 Player factory를 등록하고, Instance node도 포함하여 Field와 Instance node마다 `zlink.structure.json`에 Entry 하나를 선언한다. Framework 기본 Entry는 structure에 넣지 않는다. 처음 Field host는 하나씩 시작하고 field를 모두 만든 뒤 나오는 `READY` 줄을 확인한 다음 다음 host를 시작한다. 각 host의 `FieldCapacity`는 `FieldIds` 수와 정확히 같게 두어, 앞선 host는 가득 차고 새로 시작한 host만 선언된 field를 받게 한다.

Field·Dungeon 같은 역할 이름을 이 샘플과 다르게 정하려면 타입 이름, `GameNames`, `server/config/`, `zlink.structure.json`을 한 번에 바꾼다. 이름 변경은 기능을 옮기는 변경과 나눠서 진행하면 실패 원인을 찾기 쉽다.

옮긴 뒤 `update`가 실패하면 snapshot에 빠진 필드가 있는지 먼저 확인한다. 그다음 [디버깅](#디버깅)의 message flow로 첫 실패 단계를 찾는다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 중앙에서 고정한 공개 NuGet package를 사용하며, project reference는 이 프로젝트의 모듈 사이에 둔다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter가 직접 직렬화하는 대상은 application 상태로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

게임 사실은 그것을 변경하는 객체 한 곳에서 관리한다. 좌표와 sequence는 `PlayerActor`, 파티는 `FieldSpot`, 명단과 stage는 `DungeonSpot`의 것이다. Spot에는 플레이어 ID와 관찰용 복사 값을 보관하며, 이동 뒤에도 같은 Actor 객체 참조를 사용할 수 있다고 가정하지 않는다. request handler 안의 응답은 handler가 받은 Actor의 `BoundSession`으로 보내고, 그 밖의 push는 공개 Actor client로 player ID에 보내 delivery handler가 binding된 Session으로 전달하게 한다. Session은 인증과 binding 뒤 packet을 전달하고, 이동과 게임 판정은 해당 상태를 관리하는 Actor나 Spot에서 수행한다.

### 비동기 실행과 lifecycle

공개 비동기 호출은 템플릿처럼 await하며, handler에서 `.Wait()`, `.Result`, 동기 I/O로 실행을 중단하지 않는다. `SpotWide`가 같은 상태의 실행을 제한하므로 별도 lock이나 queue로 같은 상태를 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/dotnet/guide/server/32-execution-model/)이 설명한다.

Dungeon은 `GetOrCreate`로 만들고 Actor 이동은 `JoinSpot(...).Defer()`와 공개 완료 callback으로 처리하며, host 종료는 relocation과 runtime 관찰로 확인하여 요청 수락을 완료로 오인하지 않는다. `FieldSpot`과 `DungeonSpot`은 application이 생성하는 User Spot이며, 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/dotnet/guide/server/34-activation-lifetime/)이 설명한다. process 이름이 Instance여도 `DungeonSpot`에는 User Spot의 계약을 적용한다. Dungeon과 node의 배치가 고정되어 있다고 가정하지 않는다. runner는 Instance마다 Dungeon slot을 하나로 두므로 drain 뒤 남은 node는 가득 차고 새 Dungeon은 대체 node만 받는다.

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 좌표와 Dungeon 상태를 이어서 사용한다. Dungeon 명단과 stage는 연결이 끊겨도 유지된다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `bin/`, `obj/`는 commit에서 제외한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `WorldRun.cs`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다.

설정 파일은 `session.json`, `fieldA.json`, `fieldB.json`, `instanceA.json`, `instanceB.json`이며 교체한 node의 파일은 `instanceA-replacement`처럼 `-replacement` 접미사가 붙는다. `ManagedProcess.cs`는 진단 stderr를 `<node>.err.log`에, 준비·제어 stdout을 `<node>.out.log`에 기록한다. Redis 로그는 `redis-start.log`, `redis-port.log`, `redis.log`, `redis-stop.log`이며 로컬 빌드 출력은 `.logs/build.log`에 있다. Docker 빌드·Redis·바깥 정리 로그는 `.logs/docker-<project>/`에 있다.

### 진단 수준

`GameHosting.cs`는 메시지 수신부터 처리 완료까지의 단계를 기록하는 message flow를 다음 설정으로 활성화한다.

```csharp
options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
```

`Normal`은 receive·dispatch·completion 단계와 오류를 기록한다. 시간 상세가 필요하면 `Detailed`로 바꾸며, 크기 기록에는 `IncludeMessageSizes(true)`도 설정한다. 상세 기록에는 비용이 추가되므로 필요한 조사 범위에서 사용한다.

`NodeSettings.cs`는 diagnostics JSON key를 정의하지 않으므로 시작 설정은 hosting 호출을 수정하고 다시 빌드·시작하여 적용한다. 실행 중에는 package의 `IZLinkDiagnosticsRuntime.Level`과 `SetLevelAsync`를 사용할 수 있지만 이 프로젝트의 stdin 제어 명령에는 연결되어 있지 않다. property setter는 blocking 동작이므로 Framework handler와 callback에서는 비동기 메서드를 사용한다. 진단 항목과 설정은 [Monitoring](https://docs.zlink.systems/ko/dotnet/guide/server/26-monitoring/)과 [Observability diagnostics](https://docs.zlink.systems/ko/dotnet/reference/08-observability-diagnostics/)가 설명한다.

### Message flow 조사

`GameHosting`은 console log를 stderr로 기록하므로 message flow는 node의 `.err.log`에서 확인하며 별도 `.flow` 파일을 만들지 않는다. record의 `flow`는 node를 통과하는 메시지를 추적하는 값이고 `corr`는 request와 reply의 관계를 확인하는 값이다. record는 `zlink flow:`로 시작하며 실패는 `outcome=failed`, `error_type`, `error_message`로 표시된다.

실패 필드와 바로 앞 단계를 함께 읽고 category를 제외하지 않는다. 실행마다 flow ID가 달라지므로 통과·실패 실행은 논리적 작업, Actor ID와 단계 순서로 비교하여 처음 누락되거나 실패한 단계를 찾는다. startup이 message flow 기록 전에 실패했으면 시작 예외와 stack을 보존한다.

### 보조 trace와 정리

로컬 process 모드의 Spot discovery 추가 trace는 node를 시작하기 전 부모 환경에서 설정한다. 기본 Docker node는 호스트 환경 변수를 상속하지 않는다. PowerShell에서는 다음 환경 변수를 사용한다.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1"
```

이 값은 시작하는 child process에 적용되며 stderr를 message flow와 함께 보존한다. Bash에서는 같은 변수를 다음과 같이 설정한다.

```bash
export ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1
```

`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`는 공개 `0.30.1` package에 있는 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

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
* zlink version: <package versions from Directory.Packages.props>
* OS: <OS, .NET runtime version>

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

제목에는 언어와 실패한 동작을 적는다(예: `framework(dotnet): Spot startup fails with empty SpotId`). 등록한 issue 번호는 README의 Known issues에 적어, 수정 package가 나오면 다시 검증할 위치를 남긴다.
