# MyGame 개발 가이드

## 프로젝트 개요

이 C#/.NET 8 프로젝트는 하나의 world를 정원이 있는 Region instance로 나누어, Region이 가득 차면 같은 지역의 다음 instance를 만드는 MMO server다. 연결을 처리하는 Session gateway, Region host A·B, Dungeon을 담당하는 Instance host와 위치·relocation 정보를 관리하는 Redis를 실행한다. `Directory.Packages.props`는 ZLink Framework와 connector package를 `0.30.2`으로 고정한다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [.NET 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/), [API reference](https://docs.zlink.systems/ko/dotnet/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 이 프로젝트에 고정된 package의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 client를 모두 실행하므로 호스트 SDK가 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario region,movement,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario region,movement,dungeon,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

로컬 process 디버깅은 .NET 8 SDK를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 Debug 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가한다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다.

## 프로젝트 구성과 확장 위치

### Session gateway

`server/MyGame.Session/Program.cs`는 연결 처리를 등록하고 `server/MyGame.Session/Infrastructure/ZLink/PlayerSession.cs`는 `AllowedPackets`로 client packet 이름을 검사한 뒤 Player Actor를 binding하고 허용 패킷을 relay한다. 첫 패킷은 `EnterRegionRequest`이며 예제는 player ID를 신뢰하므로 binding 전에 인증을 추가한다. 내부 메시지는 client 허용 목록에 넣지 않는다.

### Region과 플레이어

ID로 찾는 게임 상태 객체가 Actor이며, `server/MyGame.Shared/Infrastructure/ZLink/PlayerActor.cs`의 `PlayerActor`는 게임 상태 없이 Region의 거부 응답을 Entry에 전달한다. `server/MyGame.Region/Infrastructure/ZLink/RegionSpot.cs`의 `RegionSpot`은 구성원·정원 admission·broadcast를 소유한다. `server/MyGame.Region/Domain/Positions.cs`의 `Positions`는 위치·순번·이동 제한·시간 예산을 판정한다.

`server/MyGame.Region/Program.cs`가 타입과 handler를 등록한다. `server/MyGame.Region/Infrastructure/ZLink/RegionEntry.cs`는 첫 패킷을 받아 참가할 instance를 고르고, `server/MyGame.Region/Infrastructure/ZLink/Handlers.cs`는 push를 client로 전달하는 delivery handler를 포함한 Region handler를 담고 있다. 플레이어별 규칙과 Region 공통 규칙을 연결하는 handler는 여기에 추가한다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### Dungeon

`server/MyGame.Instance/Program.cs`와 `server/MyGame.Instance/Infrastructure/ZLink/DungeonSpot.cs`는 요청에 따라 Dungeon을 생성하고 typed 정보 요청에 응답한다. `DungeonSpot`은 application이 생성하는 User Spot이며 모든 참가를 거부하고 상태를 갖지 않으며 relocation 때 다시 생성된다. 파티 참가 조건·전투·보상·Dungeon lifecycle은 이 프로젝트에 추가한다.

### 공통 계약과 hosting

`server/MyGame.Shared/Contracts.cs`는 메시지·허용 region·복제 및 이동 제한·거부 사유를 정의한다. `server/MyGame.Shared/NodeSettings.cs`는 `Capacity`와 Framework `RegionLimit`를 포함한 process 설정을 정의한다. 복제 선택은 `RegionEntry`, 이동 상태는 `Positions`가 소유한다. `GameHosting.cs`는 hosting과 진단을 설정하고 `NodeControl.cs`는 실행 중 제어 명령을 처리한다.

### E2E client

`e2e/MyGame.E2E/GameClient.cs`는 공개 connector로 접속한다. `Scenarios.cs`는 client가 관찰한 동작을 검사하고 `DomainChecks.cs`는 scenario node를 시작하기 전에 규칙 경계를 검사한다. `WorldRun.cs`는 실행별 process를 관리하고 `StructureCheck.cs`는 구성을 검사한다. `--check-domain`으로 domain 검사를 실행하며 별도 domain-test project는 두지 않는다.

## 프로젝트의 ZLink 개념

### RouteMesh

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `GameNames.WorldMesh`를 사용한다. 이 프로젝트는 application channel을 등록하지 않으므로 `zlink.structure.json`의 `channels`는 비어 있다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/dotnet/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`RegionEntry`는 첫 패킷을 받는 Entry Spot이고, `RegionSpot`은 `forest-1` 같은 instance마다 요청에 따라 생성되는 User Spot이며, `DungeonSpot`은 Instance node의 User Spot이다. [Spot 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/21-spot/)는 `RegionSpot.OpenAsync`가 Dungeon에 보내는 request를 포함해 이 객체들의 등록과 호출을 설명한다. `PlayerActor`는 Session node가 `GetOrCreate`로 요청하여 Region node에 생성되며, 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/dotnet/guide/server/23-stream/)에서 `PlayerSession`은 `AllowedPackets`를 확인하고 Player Actor를 binding한 뒤 허용된 typed packet을 전달한다. 내부 메시지는 client 허용 목록에 넣지 않는다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/dotnet/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 Redis 구현을 사용한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `RegionEntry.SelectAsync`가 `JoinSpot(...).Defer()`를 요청하고, `RegionSpot.OnActorJoinAsync`는 참가를 수락하거나 `RegionFull` 응답과 함께 거부하며, `PlayerActor.OnJoinCompletedAsync`는 거부를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/dotnet/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter는 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `RegionRelocation`은 플레이어 위치를 직렬화하고 `NodeControl`은 공개 host relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/37-relocation/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Session node에 STREAM으로 연결하고 첫 패킷 `EnterRegionRequest`를 보낸다. `PlayerSession`은 첫 패킷 타입을 확인하고 `playerId`로 Player Actor를 `GetOrCreate`한 뒤 Session에 binding하고 원래 패킷을 relay한다.
2. 새 Actor는 Entry Spot `RegionEntry`에서 시작한다. `EnterRegion`이 identity를 확인하고, `SelectAsync`가 첫 instance(`forest-1`)를 `GetOrCreate`한 뒤 `JoinSpot(...).Defer()`를 요청한다.
3. `RegionSpot.OnActorJoinAsync`는 안에 있는 플레이어가 `Capacity`명 미만이면 참가를 수락한다. 이어서 `OnJoinedActorAsync`가 모든 구성원에게 player ID로 `RegionView`를 보내고 `DeliverRegion`이 각 Actor의 binding된 Session으로 전달한다.
4. 가득 찬 Region은 `RegionFull`과 함께 참가를 거부한다. `PlayerActor.OnJoinCompletedAsync`는 응답을 Entry에 전달하고 Entry가 다음 허용 replica를 고른다. `MaxReplicasPerRegion`이 모두 가득 차면 Entry는 `RegionUnavailable`을 보낸다. Actor는 대상을 고르지 않는다.
5. `MoveRequest`는 `MovePlayer`가 받아 `Positions`에 전달한다. Domain은 Delta·Sequence, 영역 경계와 이동 예산을 검사하고 수락할 때만 상태를 바꾼다. 요청자에게는 binding된 Session으로 RegionView를 보내고 다른 구성원에게는 기존 Actor 경로로 한 번 broadcast한다. `OpenDungeonRequest`는 `OpenDungeon`이 받아 Instance node의 Dungeon을 `GetOrCreate`하고 typed request를 보낸 뒤 client에 `DungeonOpened`로 응답한다.
6. 같은 플레이어가 재접속하면 `PlayerSession`이 기존 Actor를 새 Session에 binding하고, `ReconnectRegion`이 identity를 확인한 뒤 현재 `RegionView`를 다시 보낸다. 연결이 끊긴 Session도 membership과 위치를 유지한다.
7. node를 교체할 때 `NodeControl`의 `drain`이 relocation을 요청한다. Framework는 `RegionRelocation`에서 위치·순번·남은 이동 예산 ms를 받아 다른 node에서 복원하며, Dungeon은 다시 생성된다.

## MMO 도메인 확장

이 프로젝트에 MMO 규칙과 서버 동작을 추가한다. 각 판정은 해당 상태의 소유 모듈에 두고, client가 관찰하는 동작마다 scenario를 추가한다.

### 기능 위치

| 대상 | 소유 위치 | 확장 절차 |
| --- | --- | --- |
| Typed 메시지와 공통 제한 | `server/MyGame.Shared/Contracts.cs` | Client·내부 record, 거부 사유 문자열, 허용 region 이름과 공통 수치를 여기에 추가한다. 결과마다 wire 형식을 하나만 둔다. |
| Region 위치와 이동 | `server/MyGame.Region/Domain/Positions.cs` | 위치 상태와 이동 판정을 여기에 추가한다. `Positions`가 좌표·순번·이동 예산·단조 시각·경계 검사를 소유한다. `MaxStep`은 10, X 범위는 0..100, `MaxSpeed`는 초당 `MaxStep * 5`다. 시험용 `TimeProvider`는 domain 생성자로만 전달한다. |
| Region 구성원과 전달 | `server/MyGame.Region/Infrastructure/ZLink/RegionSpot.cs` | 구성원과 Region broadcast를 여기에 둔다. Domain 상태 필드를 바꾸면 `Capture`와 `Restore`도 함께 갱신하고, process 시각 대신 남은 ms를 저장한다. 요청자에게는 binding된 Session으로 응답하고, 수락 결과는 기존 Actor 전달 경로로 나머지 구성원에게 보낸다. |
| Region 선택과 입장 | `server/MyGame.Region/Infrastructure/ZLink/RegionEntry.cs` | 허용 region 검사, 복제 instance 선택, 마지막 이용 불가 응답을 Entry 한 곳에서 판정한다. `RegionSpot`은 자신의 구성원 정원만 판정한다. `PlayerActor`는 거부된 입장을 Entry로 전달하며 다음 대상을 고르지 않는다. |
| Client 패킷 경계 | `server/MyGame.Session/Infrastructure/ZLink/PlayerSession.cs` | Client가 보내는 패킷 이름만 `AllowedPackets`에 추가한다. 내부 메시지는 허용 목록에서 제외하며, Session은 허용되지 않은 패킷에 `ActionRejected("packet-not-allowed", 0)`로 응답한다. |
| Domain·client scenario | `e2e/MyGame.E2E/DomainChecks.cs`, `e2e/MyGame.E2E/Scenarios.cs` | 각 규칙 경계를 `DomainChecks`에, 공개 동작은 이름이 있는 scenario에 추가한다. Domain 검사는 scenario node를 시작하기 전에 실행하며 `--check-domain`으로 단독 실행할 수 있다. |
| Topology와 사용자 안내 | `zlink.structure.json`, `README.md` | `run.scenarios`, scenario 선택 명령과 README scenario 표를 `Scenarios.All`과 같은 순서로 유지한다. Topology가 바뀌면 node 선언과 설정도 함께 갱신한다. |

### 변경 절차

1. Typed contract와 공통 상수를 `Contracts.cs`에 추가한다.
2. 게임 규칙과 가변 상태를 판정하는 domain 객체에 둔다. Framework callback은 `Infrastructure/ZLink` 계층에 둔다.
3. 허용 경계, 처음 거부되는 값, 거부 뒤 상태 불변을 domain 시험에 추가한다. 시간 경계는 domain 생성자로 시험 시계를 전달한다.
4. Client 패킷 이름만 `PlayerSession.AllowedPackets`에 추가한다. 내부 패킷 거부와 연결 유지를 확인한다.
5. `Scenarios.cs`에 client가 관찰하는 scenario를 추가하고 `zlink.structure.json`, README scenario 표, 두 실행 스크립트의 명령 예를 함께 갱신한다.
6. `--check-domain`, 관련 scenario, build, 전체 e2e를 실행한다. 재실행 전에 첫 실패를 확인한다.

Domain 검사는 별도 시험 project를 만들지 않고 e2e 실행 파일에 둔다. 공개 Framework API와 기본 typed JSON serializer를 사용한다. NaN이나 Infinity를 허용하는 JSON number 옵션을 추가하지 않는다.

## 개발 시 주의점

### 상태와 메시지 계약

의존성은 중앙에서 고정한 공개 NuGet package를 사용하며, project reference는 이 프로젝트의 모듈 사이에 둔다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. relocation adapter가 직접 직렬화하는 대상은 application 상태로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

게임 사실은 변경을 결정하는 객체 한 곳에서 관리한다. 인원은 `RegionSpot`, 위치·순번·이동 예산은 `Positions`가 소유하고 `PlayerActor`는 identity와 binding만 가진다. Spot 상태에는 Actor 참조 대신 player ID를 보관한다. request handler 안의 응답은 handler가 받은 Actor의 `BoundSession`으로 보내고, 그 밖의 push는 공개 Actor client로 player ID에 보내 delivery handler가 binding된 Session으로 전달하게 한다. Session은 인증과 binding 뒤 허용 목록의 client packet만 전달한다. Region 배치는 Entry가, 이동 판정은 `Positions`가 수행한다.

### 비동기 실행과 lifecycle

공개 비동기 호출은 템플릿처럼 await하며, handler에서 `.Wait()`, `.Result`, 동기 I/O로 실행을 중단하지 않는다. `SpotWide`가 같은 상태의 실행을 제한하므로 별도 lock이나 queue로 같은 상태를 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/dotnet/guide/server/32-execution-model/)이 설명한다.

Region과 Dungeon은 `GetOrCreate`로 만들고 Actor 이동은 `JoinSpot(...).Defer()`와 공개 완료 callback으로 처리하며, host 종료는 relocation과 runtime 관찰로 확인하여 요청 수락을 완료로 오인하지 않는다. `RegionSpot`과 `DungeonSpot`은 application이 생성하는 User Spot이며, 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/dotnet/guide/server/34-activation-lifetime/)이 설명한다. process 이름이 Instance여도 `DungeonSpot`에는 User Spot의 계약을 적용한다. runner는 `RegionLimit`으로 Region node마다 Region Spot을 둘로 제한하므로 drain 뒤 남은 node는 가득 차고 새 Region은 대체 node만 받는다. Region과 node의 배치가 고정되어 있다고 가정하지 않는다.

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 membership과 위치를 이어서 사용한다. 연결이 끊긴 Session도 명시적 leave 전까지 membership을 유지하며, 이미 참가한 Actor가 `EnterRegionRequest`를 반복하면 현재 Region view를 받는다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 게임 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/dotnet/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `bin/`, `obj/`는 commit에서 제외한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `WorldRun.cs`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다.

설정 파일은 `session.json`, `regionA.json`, `regionB.json`, `instance.json`이며 교체한 node의 파일은 `regionA-replacement`처럼 `-replacement` 접미사가 붙는다. `ManagedProcess.cs`는 진단 stderr를 `<node>.err.log`에, 준비·제어 stdout을 `<node>.out.log`에 기록한다. Redis 로그는 `redis-start.log`, `redis-port.log`, `redis.log`, `redis-stop.log`이며 로컬 빌드 출력은 `.logs/build.log`에 있다. Docker 빌드·Redis·바깥 정리 로그는 `.logs/docker-<project>/`에 있다.

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

`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`는 공개 `0.30.2` package에 있는 버전별 진단 switch이므로 routing API로 사용하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다.

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
