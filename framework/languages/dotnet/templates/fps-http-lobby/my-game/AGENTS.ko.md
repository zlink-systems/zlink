# MyGame FPS 개발 지침

## 프로젝트와 개발

이 템플릿의 도메인을 확장하여 FPS 서버를 만듭니다. .NET 8 프로젝트는 HTTP Lobby, Session gateway, Match host A·B, Redis Location Store를 실행합니다. Lobby는 `/match` 요청을 모드별로 매칭하고 GUID 기반 Room ID를 발급하며 동적 Room User Spot의 명단을 준비합니다. 플레이어는 STREAM으로 Session에 연결하고 배정된 Room에 입장합니다. `Directory.Packages.props`는 공개 Framework·connector package를 0.30.2로 고정합니다. 실행·배포·알려진 문제는 [README](README.md)가, 초기 구성과 시나리오 순서는 [zlink.structure.json](zlink.structure.json)이 소유합니다.

프로젝트 루트에서 Docker Compose v2와 Linux container로 실행합니다.

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

PowerShell 7에서는 같은 작업을 다음과 같이 실행합니다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

.NET 8 SDK로 host process를 디버깅할 때는 `--local` 또는 `-Local`을 추가합니다. 로컬 빌드 뒤 `--no-build` 또는 `-NoBuild`로 컴파일을 생략할 수 있습니다. 시나리오 assertion을 유지하고 재실행 전에 첫 실패를 확인합니다.

## 모듈과 확장 지점

- `server/MyGame.Shared/Contracts.cs`: typed 게임 계약, 동작 제한과 protocol 이름. `HttpLobbyContracts.cs`: HTTP lobby와 서버 전용 준비 계약. `Geometry.cs`: Vec3, 닫힌 slab ray/AABB 교차, 정적 맵, 스폰과 플레이어 상자.
- `server/MyGame.Match/Domain/RoomState.cs`: 서버 위치·HP·phase·winner·공유 순번·이동 예산·쿨다운·명중 선택. 무기·점수·대전 규칙은 이 도메인을 확장합니다. 서버 시간은 단조 시계이며 시험용 시계는 도메인 생성자에서만 주입합니다. Snapshot DTO는 process 시각 대신 남은 ms를 저장합니다.
- `server/MyGame.Lobby/Application/Matchmaker.cs`: 모드별 대기열과 매칭. `server/MyGame.Lobby/Infrastructure/ZLink/RoomProvisioner.cs`는 HTTP 응답 전에 Room을 만들고 명단을 준비합니다. `server/MyGame.Match/Domain/RoomRoster.cs`는 명단과 입장 판정을 소유합니다. 도메인에 Framework callback을 넣지 않습니다.
- `server/MyGame.Session/PlayerSession.cs`: 연결 callback, Actor binding과 client 패킷 허용 목록. Binding 전에 JoinRoomRequest의 플레이어 신원을 인증합니다. Client가 보낸 내부 준비·배정·결과 패킷을 중계하지 않습니다.
- `server/MyGame.Shared/GameHosting.cs`, `NodeSettings.cs`, `NodeControl.cs`: 공통 설정·진단·구성·drain·shutdown 관찰. Routing·membership·binding·relocation은 Framework 공개 API의 소유를 유지합니다.
- `e2e/MyGame.E2E/Scenarios.cs`: lobby·hit·wall·input·reconnect·update의 실제 client 결과. `DomainChecks.cs`와 `LobbyDomainChecks.cs`: 같은 실행 파일의 게임·HTTP lobby 도메인 경계 시험. `StructureCheck.cs`: 인프라 시작 전 descriptor·설정·계약 검사. `MatchRun.cs`·`ManagedProcess.cs`: 자신이 만든 process의 lifecycle.

## 도메인 소유와 비동기 실행

RoomState 한 곳이 이동 예산·발사 간격·최단 명중을 판정합니다. Y는 위쪽이며 맵 경계와 고정 벽 AABB는 서버 공유 상수입니다. 이동은 발 위치 선분을 검사합니다. 발 위치 Y는 항상 0이며 Delta.Y가 0이 아니면 `vertical-move`로 거부합니다. 점프·중력은 없습니다. Ray가 상자 면에 닿아도 교차이며 교차 거리가 같으면 벽을 우선합니다. 수락된 빗나감·벽 차단 사격도 순번과 쿨다운을 소비합니다. 거부된 입력은 상태와 시간을 바꾸지 않습니다. RoomView는 재접속한 새 client가 이어서 입력하도록 마지막 순번을 제공합니다.

현재 서버 상태 기준으로 명중을 판정합니다. 지연 보정·투사체·클라이언트 예측은 없습니다. Handler나 client에 두 번째 판정자를 추가하지 않고 기존 geometry·규칙·typed 계약을 확장합니다.

게임 상태는 Spot 도메인에 두고 공개 Actor·Spot API를 사용합니다. Framework가 handler turn을 직렬화하므로 같은 실행 권한을 다시 판정하는 lock·queue·generation 검사를 추가하지 않습니다. Handler turn 안에서 인프라 진행을 동기로 기다리지 않습니다. 애플리케이션 handler의 공개 비동기 호출은 허용됩니다. Relocation adapter는 게임 상태를 직렬화하고 Framework는 host 이전·location·route 연속성을 소유합니다. 명령 수락만 보지 않고 relocation 완료와 safe shutdown을 관찰합니다.

## 공개 API 문서

[ZLink 문서](https://docs.zlink.systems/), [.NET server guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), [agent index](https://docs.zlink.systems/llms.txt)를 사용합니다. 현재 문서의 API를 고정 package와 대조합니다. Entry·Spot·Actor·Session·Location·relocation은 공개 Framework API와 기본 typed serializer를 사용합니다. Reflection·private API·raw frame·애플리케이션 routing 우회를 사용하지 않습니다.

## 진단과 결함

`GameHosting.cs`는 dispatch diagnostics를 Normal로 설정하며 node stderr에 message-flow가 남습니다. 실패 수정·재실행 전에 첫 `.logs/run-<id>/` 설정·stdout·stderr를 보존합니다. 통과·실패 trace의 작업·Actor·flow/corr·transition 순서를 대조합니다. 격리 실행에서 한 번 실패하면 재실행 통과와 무관하게 결함입니다. Flow 기록 전에 시작이 실패하면 시작 예외를 보존합니다.

로컬 process의 discovery 진단은 node 시작 전에 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`을 설정합니다. Docker node는 임의의 host 환경 변수를 상속하지 않습니다. 실패를 없애려고 timeout·고정 대기·assertion을 바꾸지 않습니다. Runner는 자신이 만든 process와 Compose project만 정리합니다. 정리 오류를 보존하고 다른 작업을 중단하지 않습니다.

문서대로 호출한 공개 API가 실패하면 package 버전·OS·명령 또는 최소 공개 API 재현·기대 결과·첫 node 예외와 stack·관련 flow/corr를 보존합니다. 보고서를 제안하기 전에 [기존 issue](https://github.com/zlink-systems/zlink/issues)를 확인합니다. 자격 증명과 비공개 정보를 제거합니다. Issue 공개 전 개발자에게 초안을 보여 주며 확인된 영향 버전은 README의 Known issues에 적습니다.
