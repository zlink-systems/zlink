# MyGame 개발 가이드

## 프로젝트 개요

이 C#/.NET 8 프로젝트는 플레이어가 Zone 경계를 넘어 이동하는 MMO server 예제다. 연결을 처리하는 Session gateway, Zone host A·B, Dungeon host와 위치·이동 상태를 저장하는 Redis를 실행한다. `Directory.Packages.props`는 ZLink Framework와 connector package를 `0.30.0`으로 고정한다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [.NET 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/), [API reference](https://docs.zlink.systems/ko/dotnet/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 이 프로젝트에 고정된 package의 API와 대조한다.

## 개발 명령

### Windows PowerShell 7

README의 사전 요구 사항을 설치한 상태에서 이 디렉터리를 기준으로 실행한다. solution에는 server 역할별 프로젝트와 e2e client가 포함되므로, 컴파일된 client를 직접 실행하기 전에 빌드한다.

```powershell
dotnet build server/MyGame.sln
# Run all scenarios, including a build.
pwsh -NoProfile -File ./run-e2e.ps1
# Select scenarios in canonical order.
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario cross,border,reconnect
# Reuse the existing build.
pwsh -NoProfile -File ./run-e2e.ps1 -NoBuild
# Check structure without Docker or server startup.
dotnet e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll --root . --check-structure
```

`-NoBuild`는 기존 빌드를 사용하고, `--check-structure`는 Docker와 server를 시작하지 않고 구성을 검사한다. 변경 뒤에는 구조 검사와 영향을 받는 scenario를 실행하며, 실패는 [Known issues](README.md#known-issues--알려진-문제)와 대조한다.

### Linux·WSL Bash

Bash에서도 같은 solution과 e2e client를 사용한다. 빌드 생략과 scenario 선택을 함께 지정할 때는 runner가 첫 인자에서 빌드 생략을 확인하므로 `--no-build`를 먼저 둔다.

```bash
dotnet build server/MyGame.sln
./run-e2e.sh
./run-e2e.sh --scenario cross,border,reconnect
./run-e2e.sh --no-build
dotnet e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll --root . --check-structure
```

기본 scenario 순서는 `cross,border,reconnect,update`이며 `e2e/MyGame.E2E/Scenarios.cs`와 descriptor의 목록을 함께 유지한다. runner의 기동·정리 동작은 [Local e2e](README.md#local-e2e)가 설명한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/MyGame.Session/Program.cs`는 연결 처리를 등록하고 `server/MyGame.Session/PlayerSession.cs`는 연결 callback과 수신 packet 전달을 담당한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. 인증은 binding 전에 추가하고 게임 packet은 기존 typed relay로 전달하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다.

### Zone과 플레이어

ID로 찾는 게임 상태 객체가 Actor이며, `server/MyGame.Zone/PlayerActor.cs`의 `PlayerActor`가 플레이어의 좌표와 sequence를 관리한다. Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이며, `server/MyGame.Zone/ZoneSpot.cs`의 `ZoneSpot`이 Zone의 가시성 정보와 도착 대기 정보를 관리한다. 가시성 정보는 Actor의 이동 상태에서 복사한 관찰용 값이므로, 플레이어 상태 변경은 Actor에 적용한다.

`server/MyGame.Zone/Program.cs`가 타입과 handler를 등록하고 `server/MyGame.Zone/WorldEntry.cs`가 플레이어 Actor의 월드 진입을 처리한다. 플레이어별 규칙과 Zone 공통 규칙을 연결하는 handler는 `server/MyGame.Zone/Handlers.cs`에 추가한다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### Dungeon

`server/MyGame.Instance/Program.cs`와 `server/MyGame.Instance/DungeonSpot.cs`는 요청에 따라 Dungeon을 생성하고 정보를 반환한다. `DungeonSpot`은 application이 생성하는 User Spot이며 Framework가 별도 생성·초기화 계약으로 제공하는 Instance Spot과 구분된다. 파티 참가 조건·전투·보상·Dungeon lifecycle은 이 프로젝트에 추가한다.

### 공통 계약과 hosting

`server/MyGame.Shared/Contracts.cs`는 process 간 메시지와 게임 상수를, `server/MyGame.Shared/NodeSettings.cs`는 process 설정을 정의한다. `server/MyGame.Shared/GameHosting.cs`는 hosting과 진단을 설정하고 `server/MyGame.Shared/NodeControl.cs`는 실행 중 제어 명령을 처리한다. 여러 process가 사용하는 계약은 여기에 추가하며, 플레이어 상태는 `PlayerActor`에 유지하여 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### E2E client

`e2e/MyGame.E2E/GameClient.cs`는 공개 connector로 게임에 접속하고 `e2e/MyGame.E2E/Scenarios.cs`는 client가 관찰할 결과를 검사한다. `e2e/MyGame.E2E/WorldRun.cs`는 실행별 process와 기반 시설을 관리하고 `e2e/MyGame.E2E/StructureCheck.cs`는 구성을 검사한다. 새 게임 동작은 client가 받은 결과로 검증하며, 별도 unit-test 프로젝트와 formatter 설정은 포함되어 있지 않다.

## 프로젝트의 ZLink 개념

### RouteMesh와 channel

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `GameNames.WorldMesh`를 사용한다. 같은 목적의 메시지를 이름으로 구분하는 channel 중 `border`는 Zone 사이의 typed publication에 사용된다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/dotnet/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`WorldEntry`는 Actor 진입을 처리하고 `ZoneSpot`은 지리적 Zone을, `DungeonSpot`은 동적으로 생성한 Dungeon을 표현한다. [Spot 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/21-spot/)는 이 객체들의 등록과 호출을 설명한다. `PlayerActor`는 소속 Spot이 바뀌어도 플레이어 이동 상태를 유지하며, 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/dotnet/guide/server/23-stream/)에서 `PlayerSession`은 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/dotnet/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 Redis 구현을 사용한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `PlayerActor.Move`는 다음 Zone 참가를 지연 처리로 요청하고 `OnJoinCompletedAsync`는 완료 결과를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/dotnet/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter는 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `NodeControl`은 공개 host relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/37-relocation/)가 설명한다.

### 경계 알림과 timer

주기적으로 handler를 실행하는 timer를 사용하여 `BorderTick`이 경계 근처 플레이어 정보를 publish한다. 이 pub/sub 방식에서는 `BorderHandler`가 구독한 정보를 받고, `BorderDelivery`가 수신 Actor의 binding된 Session으로 알림을 전달한다. 주기 작업의 등록과 실행은 [Timers and workers](https://docs.zlink.systems/ko/dotnet/guide/server/36-timer-worker/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 예시를 바꾸기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 패킷 `JoinWorldRequest`를 보낸다. `PlayerSession`은 첫 패킷 타입을 확인하고 `playerId`로 Player Actor를 `GetOrCreate`한 뒤 Session에 binding하고 원래 패킷을 relay한다.
2. 새 Actor는 Entry Spot `WorldEntry`에서 시작한다. `EnterWorldHandler`가 identity를 확인하고 `PlayerActor.Move`로 좌표에 해당하는 Zone에 `JoinSpot(...).Defer()`를 요청한다.
3. 참가가 끝나면 `PlayerActor.OnJoinCompletedAsync`가 binding된 Session으로 `WorldState`를 보낸다. 이후 같은 client의 패킷은 Actor가 속한 Zone의 handler(`MoveHandler` 등)가 처리하고 결과를 같은 Session으로 보낸다.
4. 이동이 다른 Zone의 좌표에 닿으면 `MoveHandler`가 다시 `PlayerActor.Move`를 호출한다. 다른 node에 있는 Zone이어도 client 연결은 바뀌지 않는다.
5. node를 교체할 때 `NodeControl`의 `drain`이 relocation을 요청한다. Framework는 `PlayerRelocation`·`ZoneRelocation`의 `Capture`로 게임 상태를 받아 다른 node에서 `Restore`한다.

## 예시 게임 로직 교체

이 템플릿에는 구조와 운영 동작을 실제로 확인하기 위한 작은 예시 게임이 들어 있다. 예시 게임은 한 줄 좌표 위의 이동, Zone 경계 근처의 가시성 알림, Dungeon 열기다. 자기 게임을 만들 때는 이 예시 로직을 바꾸고, Session 연결·Actor와 Spot 등록·relocation·drain·e2e 실행기는 그대로 사용한다. 실행기와 relocation 틀은 게임 내용과 무관하게 서버 교체와 재접속을 검증하기 때문이다.

### 바꾸기 전 확인

교체를 시작하기 전에 [개발 명령](#개발-명령)으로 빌드와 e2e를 한 번 실행해 받은 상태가 동작하는지 확인한다. 이 결과가 이후 변경의 기준이 된다. 실패하면 먼저 [README](README.md)의 Known issues와 대조한다.

### 바꾸는 부분과 남기는 부분

| 파일 | 예시 게임 로직(바꾼다) | 구조(남긴다) |
| --- | --- | --- |
| `server/MyGame.Shared/Contracts.cs` | `WorldState`, `JoinWorldRequest`, `MoveRequest`, `EnterZone`, `BorderPresence`, `BorderVisible`, `OpenDungeonRequest`, `DungeonOpened` | `GameNames`의 등록 이름, `SessionInfoRequest`, `SessionInfo`, `NodeReady`, `NodeProtocol` |
| `server/MyGame.Zone/PlayerActor.cs` | 좌표 `X`, `Sequence`와 이동 규칙 | Actor 등록, `Capture`·`Restore`, `PlayerRelocation` |
| `server/MyGame.Zone/ZoneSpot.cs` | 가시성 정보, 도착 대기 정보, 경계 계산 | Spot 등록, `Capture`·`Restore`, `ZoneRelocation` |
| `server/MyGame.Zone/Handlers.cs` | `MoveHandler`, `BorderTick`, `BorderHandler`, `BorderDelivery`, `OpenDungeonHandler` | 첫 입장·재접속을 처리하는 `ReconnectHandler`의 identity 확인과 상태 재전송 형태 |
| `server/MyGame.Zone/WorldEntry.cs` | `EnterWorldHandler`가 받는 첫 패킷 내용 | Entry Spot 등록과 첫 입장 경로 |
| `server/MyGame.Instance/DungeonSpot.cs` | Dungeon 정보 | 동적 User Spot 생성 형태 |
| `server/MyGame.Session/`, `GameHosting.cs`, `NodeControl.cs`, `NodeSettings.cs` | — | 전부 남긴다 |

`NodeControl`의 `drain`·`shutdown`은 운영 제어이고, `vacate-zone`은 e2e가 대체 node 배치를 확인하는 데 쓰는 제어다. 둘 다 남긴다.

### 교체 순서

메시지 contract부터 바꾼다. `Contracts.cs`에서 예시 메시지를 지우고 자기 게임의 typed record를 정의한 뒤, 컴파일 오류가 나는 handler를 하나씩 새 메시지로 옮긴다. 첫 입장 패킷을 바꾸면 `PlayerSession`의 첫 패킷 검사, `EnterWorldHandler`, `ReconnectHandler`가 같은 타입을 받도록 함께 고친다.

상태는 그 상태를 결정하는 객체에 둔다. 플레이어별 상태는 `PlayerActor`, 여러 플레이어가 공유하는 상태는 Spot에 둔다. 상태 필드를 바꿀 때는 같은 객체의 snapshot record와 `Capture`·`Restore`를 함께 고친다. 빠뜨린 필드는 relocation 뒤 사라지고, update 시나리오가 그 손실을 잡는다.

경계 알림처럼 쓰지 않는 기능은 handler와 등록을 함께 지운다. `border` channel을 지우면 `ZoneSpot.Configure()`의 구독, `Program.cs`의 channel 등록, `zlink.structure.json`의 `channels` 선언을 같은 변경에서 지운다. 셋이 다르면 구조 검사가 실행 전에 실패한다.

Zone·Dungeon처럼 역할 이름을 바꾸려면 타입 이름, `GameNames`, `server/config/`, `zlink.structure.json`을 한 번에 바꾼다. 이름만 바꾸는 작업은 게임 로직 교체와 나눠서 진행하면 실패 원인을 찾기 쉽다.

### e2e 시나리오 교체

`cross`와 `border`는 예시 게임을 검사하므로 자기 게임의 시나리오로 바꾼다. `reconnect`와 `update`는 남기고 상태 비교만 새 상태로 바꾼다. 이 두 시나리오가 재접속 뒤 같은 Actor와 상태가 유지되는지, 서버 교체 뒤에도 연결·상태가 유지되고 대체 node가 새 작업을 받는지를 판정하기 때문이다.

시나리오를 더하거나 지우면 `e2e/MyGame.E2E/Scenarios.cs`의 목록과 `zlink.structure.json`의 `run.scenarios`, `run.commands`의 시나리오 선택 예시를 함께 고친다. 판정은 실제로 받은 응답과 이벤트로 하고, 고정 대기나 기한 확대로 통과시키지 않는다.

### 교체 뒤 확인

빌드, 구조 검사, 전체 e2e를 차례로 실행한다. `update`가 실패하면 snapshot에 빠진 상태가 있는지 먼저 확인한다. 그다음 [디버깅](#디버깅)의 message flow로 첫 실패 단계를 찾는다.

## 아이템 줍기 기능 추가

### 메시지와 handler

Zone 안에서 아이템을 줍는 기능은 요청·결과 계약과 Zone handler부터 추가한다. `Contracts.cs`에 새 application record `PickupRequest`와 `ItemPickedUp`을 정의하여 아이템 ID와 게임 결과를 표현하며, 요청자 identity는 packet의 자기 신고 값 대신 handler의 Actor 인자에서 확인한다.

`Handlers.cs`에 새 `PickupHandler`를 추가할 때는 기존 `MoveHandler`의 `ZLinkSpotActorSendHandler` attribute, `IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, MoveRequest>` interface와 `HandleAsync` signature를 기준으로 한다. attribute의 packet 이름과 interface의 메시지 타입을 새 요청에 맞추며, `Program.cs`에는 다음 assembly scan 호출이 등록되어 있다.

```csharp
options.AddHandlersFromAssemblyOf<ZoneSpot>();
```

이 호출은 `ZoneSpot`과 같은 assembly의 handler를 등록하므로 새 handler도 그 assembly에 둔다. `PickupRequest`, `ItemPickedUp`, `PickupHandler`는 이 기능을 위해 추가할 게임 타입이다.

### 상태 변경과 결과 전달

handler 또는 callback이 상태를 변경할 실행 구간이 turn이며, 같은 상태에 대한 turn의 동시 실행을 제한하는 장치가 execution gate다. 이 프로젝트의 `SpotWide` 설정은 같은 Zone의 Spot·멤버 Actor handler, timer와 lifecycle callback이 공통 gate를 사용하게 한다. 아이템 handler는 같은 turn에서 `ZoneSpot`의 바닥 아이템과 `PlayerActor`의 inventory를 변경하여 중복 습득을 판정한다.

아이템이 있고 `PlayerActor.X`를 기준으로 거리가 허용 범위이면 바닥 아이템을 제거하고 inventory에 추가한다. 아이템이 없거나 거리가 허용 범위를 벗어나면 어느 상태도 변경하지 않고 거부 결과를 보낸다. 결과 전송은 `BorderDelivery`에서 사용하는 다음 typed Session send 형태를 따른다.

```csharp
await actor.Context.BoundSession.Send(presence).Async();
```

`presence`는 기존 코드의 메시지이며, 아이템 handler에서는 작성한 습득 결과를 전달한다. 다른 플레이어에게 알릴 때는 ID를 보관하고 `ZoneSpot.ShowBorderAsync`처럼 공개 Actor client로 전송한 뒤 `BorderDelivery` 같은 수신 handler에서 Session에 전달한다.

### 상태 보존과 검증

inventory는 `PlayerActor.cs`의 `PlayerSnapshot`, `Capture`, `Restore`에 추가하고 남은 바닥 아이템은 `ZoneSpot.cs`의 `ZoneSnapshot`, `Capture`, `Restore`에 추가한다. `PlayerRelocation`과 `ZoneRelocation`이 이 snapshot을 직렬화하므로 게임 상태만 포함한다. route·lease·membership·queue는 Framework가 관리하므로 application snapshot에 복제하지 않는다.

순수 게임 규칙을 unit test로 분리하면 습득 성공·없는 아이템·중복 습득을 검사한다. E2E에서는 `GameClient.cs`의 `MoveAsync`처럼 typed 결과 대기를 전송 전에 등록하여 빠른 응답도 관찰한다. `Scenarios.cs`의 scenario와 선택 분기, descriptor의 `run.scenarios`를 함께 확장하고 기존 scenario와 assertion을 유지한다.

### 검증 실행

검증은 수신 결과·중복 거부·reconnect와 relocation 뒤 inventory 보존을 관찰한다. 빌드와 구조 검사 뒤 관련 scenario를 실행하고 첫 실패를 보존하여 application 사용 오류인지 README의 package 결함인지 구분한다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 중앙에서 고정한 공개 NuGet package를 사용하며, project reference는 이 프로젝트의 모듈 사이에 둔다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter가 직접 직렬화하는 대상은 application snapshot으로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

플레이어 좌표 같은 게임 사실은 그것을 변경하는 객체 한 곳에서 관리한다. Spot에는 플레이어 ID와 관찰용 복사 값을 보관하며, 이동 뒤에도 같은 Actor 객체 참조를 사용할 수 있다고 가정하지 않는다. Session은 인증과 binding 뒤 packet을 전달하고, 이동과 게임 판정은 해당 상태를 관리하는 Actor나 Spot에서 수행한다.

### 비동기 실행과 lifecycle

공개 비동기 호출은 템플릿처럼 await하며, handler에서 `.Wait()`, `.Result`, 동기 I/O로 실행을 중단하지 않는다. `SpotWide`가 같은 상태의 실행을 제한하므로 별도 lock이나 queue로 같은 상태를 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/dotnet/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 각각 정해진 lifecycle 단계에 둔다. `DungeonSpot`의 process 이름이 Instance여도 User Spot의 계약을 적용하며, 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/dotnet/guide/server/34-activation-lifetime/)이 설명한다. Zone 변경은 `JoinSpot(...).Defer()`와 공개 완료 callback을 사용하고 host 종료는 relocation과 runtime 관찰로 확인하여 요청 수락을 완료로 오인하지 않는다.

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 플레이어 상태를 이어서 사용한다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `bin/`, `obj/`는 commit에서 제외한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `WorldRun.cs`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다.

설정 파일은 `zone-a.json`, `zone-b.json`, `instance.json`, `session.json`이며 교체한 A의 파일 prefix는 `zone-a-replacement`다. `ManagedProcess.cs`는 진단 stderr를 `<node>.err.log`에, 준비·제어 stdout을 `<node>.out.log`에 기록한다. Redis 로그는 `redis-start.log`, `redis-port.log`, `redis.log`, `redis-stop.log`이며 shell runner의 빌드 출력은 `.logs/build.log`에 있다.

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

Spot discovery의 추가 trace는 node를 시작하기 전 부모 환경에서 설정한다. PowerShell에서는 다음 환경 변수를 사용한다.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1"
```

이 값은 시작하는 child process에 적용되며 stderr를 message flow와 함께 보존한다. Bash에서는 같은 변수를 다음과 같이 설정한다.

```bash
export ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1
```

`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`는 공개 `0.30.0` package에 있는 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

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
