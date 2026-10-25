# MyGame FPS 개발 지침

## 프로젝트와 개발

이 C++20 FPS 템플릿의 게임 규칙을 확장합니다. HTTP Lobby, Session gateway, Match host A·B, Redis Location Store를 실행합니다. Lobby는 `POST /match` 요청을 mode별로 매칭하고 설정된 prefix와 UUID로 Room ID를 만들며, `RoomPrepared`를 받은 뒤 Room ID와 Session endpoint를 반환합니다. 게임의 첫 STREAM packet은 `JoinRoomRequest`이며 `RoomRoster`가 입장 여부를 결정합니다. 공개 Framework·connector package는 `bootstrap.cmake`의 0.30.2에 고정되어 있습니다. 실행·배포·알려진 문제는 [README](README.md)가, topology와 scenario 순서는 [zlink.structure.json](zlink.structure.json)이 소유합니다.

Docker Compose v2와 Linux container를 준비한 뒤 프로젝트 루트에서 실행합니다.

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
```

PowerShell 7에서는 다음 명령을 사용합니다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

호스트 process 디버깅은 C++20 compiler(GCC 13 이상 또는 Visual Studio 2022 17.4 이상), CMake 3.24 이상과 Ninja를 설치합니다. Windows에서는 Visual Studio 개발자 셸을 사용합니다. `--local` 또는 `-Local`을 지정하고, 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가합니다. Runner는 scenario 전에 게임 도메인 45개와 HTTP lobby 도메인 19개를 실행합니다. 로컬 빌드 뒤 `ctest --test-dir build --output-on-failure`로 두 domain target을 실행합니다.

## 모듈과 확장 위치

- `server/shared/contracts.hpp`와 `server/match/domain.hpp`: 게임 계약·한도·geometry·map·spawn·player box·`RoomState`. Entry와 HTTP 템플릿의 두 파일은 내용이 같아야 합니다.
- `server/shared/http_lobby_contracts.hpp`: HTTP·서버 전용 preparation 계약. `server/shared/hosting.hpp`는 HTTP readiness를 게시하고 이를 client packet allowlist에 넣지 않습니다.
- `server/lobby/matchmaker.hpp`: mode별 queue·pairing과 Room ID를 발급하는 유일한 위치. ID는 `RoomPrefix`와 생성 UUID를 이어 붙이며 counter를 쓰지 않습니다.
- `server/lobby/http.hpp`: HTTP endpoint와 Room 준비. Lobby는 `PrepareRoom(MatchId, Members)`를 보내고 `RoomPrepared(MatchId)`를 받은 뒤 응답합니다.
- `server/match/roster.hpp`: roster 검사·멱등 preparation·admission·relocation snapshot 상태. 준비된 명단은 다른 명단으로 교체할 수 없습니다.
- `server/match/room.hpp`와 `server/match/player.hpp`: Room handler·Player Actor·재접속·relocation. Framework callback을 게임 domain에 넣지 않습니다.
- `server/session/session.hpp`: 연결 callback·Actor binding·client packet allowlist. Binding 전에 `JoinRoomRequest.PlayerId`를 인증합니다. Client의 `PrepareRoom`, `RoomPrepared`, `HttpNodeReady`와 내부 결과 packet을 relay하지 않습니다.
- `server/shared/config.hpp`와 `server/shared/hosting.hpp`: process 설정·진단·drain·shutdown 관찰.
- `e2e/scenarios.hpp`: lobby·hit·wall·input·reconnect·update의 client 관찰. `e2e/domain_test.cpp`와 `e2e/lobby_domain_test.cpp`는 scenario 전에 실행하고 CTest target으로도 제공합니다. `e2e/structure.hpp`는 인프라 시작 전에 descriptor와 설정을 확인합니다. `e2e/client.hpp`·`process.hpp`·`world.hpp`는 client·process·인프라 수명을 관리합니다.

## Domain 소유와 비동기 실행

`RoomState` 한 곳이 이동 예산·발사 간격·명중 선택·HP·입력 순번·대전 종료를 판정합니다. Y가 위쪽이며 map 경계와 두 벽 AABB는 서버 상수입니다. 이동은 발 위치 선분과 닫힌 벽 상자를 검사합니다. 발 위치 Y는 0이며 `Delta.Y`가 0이 아니면 `vertical-move`로 거부합니다. 점프와 중력은 없습니다. 상자 면에 닿아도 교차이며 거리가 같으면 벽을 우선합니다. 수락된 빗나감과 벽 차단 사격도 순번·cooldown을 사용합니다. 거부된 입력은 게임 상태와 시간을 바꾸지 않습니다. Snapshot은 절대 process 시각을 저장하지 않고 남은 ms를 저장합니다.

`RoomRoster`는 준비된 명단과 admission을 소유합니다. Lobby는 짝을 이룬 Room에 명단 하나를 전달합니다. 같은 preparation은 멱등이며 다른 명단은 거부합니다. Relocation은 `{Game, Roster}`를 저장하여 이동 뒤에도 명단과 전투 상태를 유지합니다. `Matchmaker`는 mode 분리와 Room ID 발급을 소유합니다. 아직 짝을 이루지 않은 HTTP 요청은 Lobby process 메모리에 남으며 Match host와 함께 이동하지 않습니다.

게임 상태는 Spot domain에 두고 공개 Actor·Spot·RouteMesh API를 사용합니다. Framework가 handler turn을 직렬화하므로 같은 실행 권한을 다시 판정하는 lock·queue·generation 검사를 추가하지 않습니다. Handler turn 안에서 인프라 진행을 동기로 기다리지 않습니다. Relocation adapter는 application 상태를 직렬화하고 host 이전·membership·location·route 연속성은 Framework가 소유합니다. 명령을 보냈다는 사실만 확인하지 말고 relocation 완료와 safe shutdown을 관찰합니다.

## 공개 API 문서

[ZLink 문서](https://docs.zlink.systems/), [C++ server guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/), [agent index](https://docs.zlink.systems/llms.txt)를 사용합니다. `bootstrap.cmake`에 고정한 package 버전과 현재 문서를 대조합니다. Entry·Spot·Actor·Session·Location·relocation은 공개 Framework API와 기본 typed serializer를 사용합니다. Private API·raw frame 해석·수동 JSON 변환·application routing 우회를 추가하지 않습니다.

고정된 0.30.2 C++ public header는 문서의 typed `SessionInfoRequest` reply 예시와 맞지 않습니다. STREAM write/reply는 `zlink::message_t`를 받지만 `fx::message_t::from(T)`는 typed wrapper를 반환하고, `submit()`은 `void`입니다. Request/reply 의미를 push로 바꾸거나 serializer 우회를 넣지 말고 public 계약 차이로 기록합니다.

## 진단과 결함

`server/shared/hosting.hpp`는 dispatch diagnostics를 Normal로 설정하며 node stderr에 message-flow가 남습니다. 실패 수정·재실행 전에 첫 `.logs/run-<id>/` 설정·stdout·stderr를 보존합니다. 통과·실패 trace의 작업·Actor·flow/corr·transition 순서를 대조합니다. 격리 실행에서 한 번 실패하면 재실행 통과와 무관하게 결함입니다. Flow 기록 전에 시작이 실패하면 시작 예외를 보존합니다.

로컬 Spot discovery 진단은 node 시작 전에 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`을 설정합니다. Docker node는 임의의 host 환경 변수를 상속하지 않습니다. 실패를 없애려고 timeout·고정 대기·assertion을 바꾸지 않습니다. Runner는 자신이 만든 process와 Compose project만 정리합니다. 정리 오류를 보존하고 다른 작업을 중단하지 않습니다.

문서대로 호출한 public API가 실패하면 package 버전·OS·명령 또는 최소 공개 API 재현·기대 결과·첫 node 예외와 관련 flow/corr를 보존합니다. Public guide와 실제 header signature를 대조하고 public 계약 불일치를 application workaround로 숨기지 않습니다.
