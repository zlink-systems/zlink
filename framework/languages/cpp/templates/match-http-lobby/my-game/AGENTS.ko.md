# MyGame 개발 가이드

## 프로젝트 개요

이 C++20 프로젝트는 HTTP 매칭 로비가 두 플레이어를 짝짓고 ZLink STREAM이 게임 플레이를 전달하는 아레나 예제다. HTTP Lobby node, 연결을 처리하는 Session gateway, Room Spot과 Player Actor를 호스팅하는 Match node A·B와 위치·이동 상태를 저장하는 Redis를 실행한다. `bootstrap.cmake`는 `ZLINK_FRAMEWORK_CPP_VERSION`이 가리키는 버전의 ZLink Framework prebuilt를 설치하며, 이 변수가 Framework 버전을 정하는 유일한 곳이다. package manager는 실행하지 않는다.

게임 기능은 아래의 프로젝트 구성과 확장 절차에 따라 추가한다. 실행 환경·배포·공개 package의 알려진 문제는 [README](README.md)가 설명한다. 초기 node 구성과 scenario 목록은 [zlink.structure.json](zlink.structure.json)에 있으며, 실행 중 객체의 위치는 Framework와 Redis가 관리한다.

> **문서와 package 버전.** [ZLink 문서](https://docs.zlink.systems/ko/), [C++ 가이드](https://docs.zlink.systems/ko/cpp/guide/server/), [API reference](https://docs.zlink.systems/ko/cpp/reference/)와 [에이전트 색인](https://docs.zlink.systems/llms.txt)이 공개 문서의 진입점이다. 현재 가이드가 더 새 버전을 설명하면 `bootstrap.cmake`에 고정된 Framework 버전의 API와 대조한다.

## 개발 명령

기본 실행은 Docker에서 node와 runner를 모두 실행하므로 호스트 toolchain이 필요 없다. 이 디렉터리에서 다음 명령을 실행한다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 --check-structure
```

Bash도 같은 절차를 실행한다.

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,reconnect,update
./run-e2e.sh --check-structure
```

로컬 process 디버깅은 C++20 컴파일러(GCC 13 이상 또는 Visual Studio 2022 17.4 이상이며 Windows에서는 Visual Studio 개발자 셸), CMake 3.24 이상과 Ninja를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 로컬 모드는 `cmake -P bootstrap.cmake`, `cmake --preset default`, `cmake --build --preset default`를 실행한 뒤 `build/bin/my-game-e2e`를 시작한다. 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가한다. 시나리오 순서와 assertion은 유지하며 최초 실패를 README의 Known issues와 대조한다. Room 규칙 테스트와 HTTP 연결 끊김·재참가 회귀 테스트(`e2e/domain_tests.cpp`로 빌드되는 `room-rules`)는 프로젝트와 함께 빌드되며, 로컬 빌드 뒤 `ctest --test-dir build --output-on-failure`로 실행한다.

## 프로젝트 구성과 확장 위치

### HTTP 로비

`server/lobby/main.cpp`는 Lobby node를 RouteMesh client로 등록하고 `server/lobby/http.hpp`의 `HttpLobby`를 hosted service로 추가한다. `HttpLobby`는 `MatchRequest`(`playerId`, `mode`)를 담은 `POST /match`를 받아, 같은 mode를 요청하는 다른 플레이어가 올 때까지 첫 응답을 붙들고 있다가 새 Room을 `get_or_create`한 뒤 두 플레이어에게 `MatchResponse`(`roomId`, `sessionEndpoint`)로 답한다. HTTP listener는 `server/lobby/http.hpp`와 `server/shared/http.hpp`에서 Boost.Beast로 작성되어 있으며, Framework 자체의 HTTP 표면은 [HTTP hosting 가이드](https://docs.zlink.systems/ko/cpp/guide/server/42-http-hosting/)가 설명한다. lobby는 대기열을 process 메모리에 두며 relocation 참가자가 아니다. 운영용 인증과 내구성 있는 매칭은 이 node의 확장 지점이다.

### Session gateway

`server/session/main.cpp`는 STREAM listener와 `Session`을 등록하고 `server/session/session.hpp`의 `Session::on_packet`은 연결 callback과 수신 packet 전달을 담당한다. 연결별 통신 객체가 Session이며, Framework는 Session을 플레이어 상태 객체와 연결하는 binding을 제공한다. 인증은 binding 전에 추가하고 게임 packet은 기존 typed relay로 전달하여 플레이어에게 응답할 연결을 Framework가 확인하게 한다. C++ Session은 자신이 만든 typed reply를 아직 보낼 수 없으므로, `Session`은 `SessionInfoRequest`에 binding된 Actor에서 만든 `SessionInfo` push로 답한다(README의 Known issues 참조).

### Room과 플레이어

ID로 찾는 게임 상태 객체가 Actor이며, `server/match/player.hpp`의 `Player`는 상태가 없고 거부된 참가를 Session에 알리는 Actor다. Actor를 받아 공통 게임 규칙을 실행하는 상태 객체가 Spot이며, `server/match/room.hpp`의 `Room`이 `server/match/domain.hpp`의 `RoomState`로 전투 상태를 관리한다. 이 타입은 참가 수락 규칙, 닫힌 벽과 사거리 검사, sequence 검사와 승자를 가진다. 플레이어별 HP와 위치는 Room에 있으므로 플레이어 상태 변경은 `RoomState`에 적용한다.

`server/match/main.cpp`가 `Entry`(`server/match/player.hpp`에 정의), `Player` factory, `RoomCapacity` stable type limit을 지정한 `Room` factory를 등록한다. 전투 규칙을 적용하는 handler는 `Room::configure()`에서 `add_actor_send`로 추가한다. node 구성을 바꾸면 실제 등록과 `server/config/`, descriptor를 함께 맞추어 구조 검사가 같은 구성을 검사하게 한다.

### 공통 계약과 hosting

`server/shared/contracts.hpp`는 `Names`·`Game`·`Rejection`·`Limits`·`Control` 상수와 process 간 메시지를, `server/shared/config.hpp`는 `server/config/`에서 읽는 process 설정 `NodeConfig`를 정의한다. `server/shared/hosting.hpp`의 `configure_host`는 hosting과 진단을 설정하고 같은 파일의 `NodeControl`은 실행 중 제어 명령을 처리한다. 여러 process가 사용하는 계약은 `contracts.hpp`에 추가하며, 전투 상태는 `RoomState`에 유지하여 서로 다른 process가 같은 값을 따로 변경하지 않게 한다.

### 빌드와 e2e runner

`CMakeLists.txt`는 role마다 `server/<role>/main.cpp`로 실행 파일(`my-game-session`, `my-game-match`, `my-game-lobby`)을 만들고 `my-game-e2e` runner를 만든다. node role을 추가하면 `foreach(role ...)` 목록에 넣는다. `e2e/client.hpp`는 공개 stream connector로 게임에 접속하고 `e2e/scenarios.hpp`는 client가 관찰할 결과를 검사한다. `e2e/world.hpp`는 `e2e/process.hpp`로 node와 기반 시설을 시작하고, `e2e/main.cpp`는 시나리오를 선택해 실행하며, `e2e/structure.hpp`는 `zlink.structure.json`을 `server/config/`와 `contracts.hpp`의 이름에 대조해 검사한다. 새 게임 동작은 client가 받은 결과로 검증하며, Room 규칙과 lobby의 HTTP 처리는 별도 unit test target `my-game-rules`가 검사한다. formatter 설정은 포함되어 있지 않다.

## 프로젝트의 ZLink 개념

### RouteMesh

process 간 메시지를 전달할 node들을 연결하는 구성이 RouteMesh이며, 이 프로젝트에서는 `add_route_mesh`로 `Names::mesh` (`my-game-arena`)를 사용한다. 등록과 호출은 [Channel messaging](https://docs.zlink.systems/ko/cpp/guide/server/20-channel-messaging/)에서 설명한다.

### Spot과 Actor

`Entry`는 Actor 진입을 처리하고 `Room`은 동적으로 생성한 대전 Room 하나를 표현한다. [Spot 가이드](https://docs.zlink.systems/ko/cpp/guide/server/21-spot/)는 이 객체들의 등록과 호출을 설명한다. `Player`는 자체 application 상태가 없으므로 Actor가 이동해도 플레이어의 HP와 위치는 Room에 남는다. 생성과 호출은 [Actor 가이드](https://docs.zlink.systems/ko/cpp/guide/server/22-actor/)에서 설명한다.

### STREAM과 Session

client 연결의 packet을 처리하는 [STREAM](https://docs.zlink.systems/ko/cpp/guide/server/23-stream/)에서 `Session`은 Player Actor를 구해 binding한 뒤 원래 typed packet을 전달한다. Actor handler는 binding된 Session을 통해 client에 메시지를 보낸다. 연결과 Actor 사이의 관계는 [Session and Actor](https://docs.zlink.systems/ko/cpp/guide/server/24-actor-session/)가 설명한다.

### Location Store

객체 ID에 해당하는 현재 실행 위치를 저장하는 저장소가 Location Store이며, 이 프로젝트는 `configure_host`에서 설정한 Redis 구현을 사용한다. application은 node 주소 대신 Actor와 Spot의 ID로 호출하므로 위치가 바뀌어도 호출 대상을 같은 ID로 지정한다. 조회 방식은 [Location 가이드](https://docs.zlink.systems/ko/cpp/guide/server/25-location/)가 설명한다.

### Actor membership

Actor가 소속 Spot을 바꾸는 동작은 membership 변경이다. 대상 Spot이 참가 요청의 수락 여부를 정하는 단계가 admission이다. `Entry::join`은 `join_spot(...).defer()`로 `roomId`가 가리키는 Room 참가를 지연 처리로 요청하고, `Room::on_actor_join`이 `RoomState::admit`으로 수락 여부를 정하며, `Player::on_join_completed`는 거부된 결과를 확인한다. 참가 수락과 완료의 순서는 [Actor membership](https://docs.zlink.systems/ko/cpp/guide/server/35-actor-membership/)이 설명한다.

### Host relocation과 drain

객체의 실행 host를 변경하는 동작은 relocation이며, host를 종료할 수 있도록 workload를 다른 host로 이동시키는 절차는 drain이다. application adapter는 게임 상태를 저장하고 복원하며 Framework는 workload와 route를 이전한다. `NodeControl`은 공개 host relocation을 요청한 뒤 종료 가능한 상태를 관찰하며, 절차는 [Relocation 가이드](https://docs.zlink.systems/ko/cpp/guide/server/37-relocation/)가 설명한다.

## 요청 흐름

예시 게임의 요청은 다음 경로로 처리된다. 기능을 더하거나 옮기기 전에 이 경로를 따라 코드를 읽으면 바꿀 위치를 빠르게 찾을 수 있다.

1. client가 Lobby node의 `POST /match`에 `{"playerId":"p1","mode":"duel"}`를 보낸다(`HttpLobby::match`). 같은 mode를 요청하는 다른 플레이어가 올 때까지 첫 응답은 보류된다.
2. 같은 mode의 두 플레이어가 대기하면 `HttpLobby`가 Room 번호를 매기고 Spot manager로 Room을 `get_or_create`한 뒤, `roomId`와 `lobby.json`에 설정한 `SessionEndpoint`를 담은 `MatchResponse`로 두 요청에 답한다.
3. 각 client는 그 Session endpoint에 STREAM으로 연결하고 첫 패킷 `JoinRoomRequest`를 보낸다. `Session::on_packet`은 첫 패킷 이름을 확인하고 `playerId`로 Player Actor를 `get_or_create`한 뒤 `bind_or_get`으로 binding하고 원래 패킷을 relay한다.
4. 새 Actor는 Entry Spot `Entry`에서 시작한다. `Entry::join`이 identity를 확인하고 Room에 `join_spot(...).defer()`를 요청한다. `Room::on_actor_join`이 플레이어를 수락하고 `Room::on_actor_joined`가 Actor client로 플레이어에게 `RoomView`를 broadcast하며, `Room::deliver_view`가 이를 binding된 Session으로 전달한다.
5. `MoveRequest`와 `ShootRequest`는 `Room::move`와 `Room::shoot`가 처리한다. `RoomState` 규칙을 적용하고 거부는 `ActionRejected`나 `ShotRejected`로 답하며, 새 `RoomView`와 승자가 나오면 `MatchEnded`(`Room::deliver_end`가 전달)를 broadcast한다.
6. node를 교체할 때 `NodeControl`의 `drain`이 relocation을 요청한다. Framework는 `RoomRelocation`의 `capture`로 게임 상태를 받아 다른 node에서 `restore`한다. `PlayerRelocation`은 상태를 옮기지 않는다.

## 이 샘플을 보며 개발하기

이 샘플은 기능들이 함께 동작하는 방식을 보여 주므로, 자기 게임으로 바꾸지 않고 읽고 실행하는 용도로 쓴다. 자기 프로젝트는 C++ 기본 셋업에서 시작한다. [ZLink Start](https://zlink.systems/start)의 Initializer에서 C++를 고르거나 ZLink 저장소의 `framework/languages/cpp/templates/starter/my-game`을 사용한다. 그다음 필요한 기능을 이 샘플에서 하나씩 옮긴다. 구조 선택은 기본 셋업의 `AGENTS.md`가 설명하며, 이 절은 `match-http-lobby`만 다룬다. 이 샘플은 바꾸지 않고 프로젝트 옆에 참고용으로 둔다.

### 샘플 먼저 실행하기

옮기기 전에 [개발 명령](#개발-명령)으로 빌드와 e2e를 한 번 실행해 받은 샘플이 동작하는지 확인하고 동작을 본다. 이 결과가 이후 비교의 기준이 된다. 실패하면 먼저 [README](README.md)의 Known issues와 대조한다.

### 옮길 기능

기능 이름은 기본 셋업 `AGENTS.md`의 기능 표와 같다(동적 Room은 이 구조에만 있는 기능이다). 경로는 이 디렉터리를 기준으로 한다. 기본 셋업에는 Session node 하나와 `connect` 시나리오만 있으므로, 마지막 열은 기능마다 기본 셋업에서 함께 늘릴 것이다.

| 기능 | 이 샘플의 파일 | 옮길 것 | 기본 셋업에서 함께 늘릴 것 |
| --- | --- | --- | --- |
| Typed 메시지 | `server/shared/contracts.hpp` | 사용할 기능의 struct: `JoinRoomRequest`, `MoveRequest`, `ShootRequest`, `RoomViewRequest`, `RoomView`, `PlayerView`, `ShotRejected`, `ActionRejected`, `MatchEnded`. `MatchRequest`·`MatchResponse`는 HTTP body다. `SessionInfoRequest`·`SessionInfo`는 client가 필요로 할 때만 옮긴다 | 메시지가 가리키는 `Names`·`Game`·`Rejection` 상수 |
| 테이블 Actor | `server/match/player.hpp`의 `Player`, `PlayerFactory` | 최소 플레이어 Actor. 거부된 참가를 알리는 `Player::on_join_completed`와 이를 만드는 `PlayerFactory` | 새 `server/match/` role과 그 `main.cpp`, `server/config/` 파일, `CMakeLists.txt` role 목록의 항목. `main.cpp`의 Actor factory 등록. `zlink.structure.json`의 `nodes` 선언과 `e2e/structure.hpp`의 구조 검사 |
| 공유 Spot | `server/match/room.hpp`의 `Room`, `server/match/domain.hpp`의 `RoomState` | `RoomState`의 참가 수락 규칙·이동과 사격 규칙·승자 계산을 가진 `Room`과 `deliver_*` forwarder | `main.cpp`의 `RoomCapacity`를 지정한 Spot factory 등록. 구조 검사가 node 설정과 대조하는 `zlink.structure.json`의 `spots` 선언 |
| Entry·첫 입장 | `server/match/player.hpp`의 `Entry`, `server/match/room.hpp`의 `Room::reconnect` | `Entry::join`(identity 확인, Room으로의 지연 참가)과 `Room::reconnect`의 identity 확인과 view 재전송 | `add_entry_spot` 등록과 `entry` 선언. 첫 입장 시나리오(`hit`)와 재접속 뒤 상태를 비교하는 `reconnect` 시나리오 |
| Session binding | `server/session/session.hpp` | 첫 패킷 검사, Actor 조회·생성, `bind_or_get` binding과 원래 typed 패킷 relay | `Entry::join`와 같은 첫 패킷 타입. 기본 셋업의 `server/session/session.hpp`에 옮긴다 |
| Host relocation | `server/match/player.hpp`의 `PlayerRelocation`, `server/match/room.hpp`의 `RoomRelocation`, `server/shared/hosting.hpp`의 `NodeControl` | `RoomState`의 `capture`·`restore`를 가진 adapter, 상태 없는 Actor를 위한 `PlayerRelocation`, `drain`·`shutdown` 제어 | `main.cpp`의 `preserve_state_with` 등록. 서버 교체 뒤에도 연결·상태가 유지되고 대체 node가 새 작업을 받는지 판정하는 `update` 시나리오 |
| HTTP 로비 | `server/lobby/http.hpp`의 `HttpLobby`, `server/lobby/main.cpp`, `server/shared/http.hpp` | 같은 mode끼리 짝짓는 `POST /match` endpoint, 대기 중 client 연결이 끊기면 요청을 제거하는 동작, Room 준비 | 새 `server/lobby/` role과 그 설정(`HttpEndpoint`, `SessionEndpoint`), `CMakeLists.txt` 항목. `zlink.structure.json`의 `lobby` node. `lobby` 시나리오 |
| 동적 Room | `server/lobby/http.hpp`의 `HttpLobby`, `server/match/main.cpp`의 `Room` factory | 동적 User Spot 생성 형태. 이름 할당, Spot manager로 `Room` `get_or_create`, 이름을 client에 반환 | `RoomCapacity`를 지정한 `Room` factory 등록. `zlink.structure.json`의 `room` Spot 선언 |

`drain`과 `shutdown`은 `NodeControl`과 함께 옮긴다.

### 옮기는 순서와 규칙

기능은 표의 행 하나씩 옮기고, 하나를 옮길 때마다 빌드, 구조 검사, 관련 e2e 시나리오를 실행한다. 시나리오 ID는 `e2e/structure.hpp`의 `Scenario` enum과 `scenario_order`, `e2e/main.cpp`의 `switch`와 usage 메시지, `zlink.structure.json`의 `run.scenarios`, `run.commands`의 시나리오 선택 예시에 함께 추가한다. 판정은 실제로 받은 응답과 이벤트로 하고, 고정 대기나 기한 확대로 통과시키지 않는다.

메시지 contract부터 옮긴다. `server/shared/contracts.hpp`에 packet struct를 `packet_name`과 JSON 변환을 갖춰 먼저 정의한 뒤, 컴파일 오류가 나는 handler를 하나씩 새 메시지로 옮긴다. 첫 입장 패킷을 바꾸면 `server/session/session.hpp`의 `Session::on_packet`에 있는 첫 패킷 검사와 `Entry::join`, `Room::reconnect`가 같은 타입을 받도록 함께 고친다.

상태는 그 상태를 결정하는 객체에 둔다. HP·위치·sequence는 `RoomState`를 통해 `Room`에 두며 `Player`는 application 상태를 갖지 않고 `HttpLobby`는 일시적인 대기열만 가진다. 상태 필드를 바꿀 때는 같은 객체의 상태 struct와 JSON 변환, relocation adapter의 `capture`·`restore`를 함께 고친다. 빠뜨린 필드는 relocation 뒤 사라지고, `update` 시나리오가 그 손실을 잡는다.

`HttpEndpoint`는 port 0으로 설정하고 실제 port는 lobby의 `READY` 줄에서 읽으며, lobby의 `SessionEndpoint`에는 Session node가 게시한 endpoint를 설정한다. `e2e/world.hpp`가 실행마다 두 가지를 모두 수행한다.

Lobby·Match·Room 같은 역할 이름을 이 샘플과 다르게 정하려면 타입 이름, `Names`, `server/config/`, `zlink.structure.json`, `CMakeLists.txt`의 role 목록을 한 번에 바꾼다. 이름 변경은 기능을 옮기는 변경과 나눠서 진행하면 실패 원인을 찾기 쉽다.

옮긴 뒤 `update`가 실패하면 snapshot에 빠진 상태가 있는지 먼저 확인한다. 그다음 [디버깅](#디버깅)의 message flow로 첫 실패 단계를 찾는다.

## 개발 시 주의점

### 상태와 메시지 계약

`bootstrap.cmake`가 고정 버전으로 설치하는 공개 Framework prebuilt를 사용하며, 저장소 소스나 로컬 feed를 프로젝트에 추가하지 않는다. typed 메시지는 기본 Framework JSON serializer로 처리하여 process마다 같은 계약을 사용한다. 메시지는 `server/shared/contracts.hpp`에 `packet_name`과 JSON 변환을 갖춰 선언하고, 메시지별 codec·수동 encode·raw frame 해석은 추가하지 않는다. relocation adapter가 직접 직렬화하는 대상은 application 상태(`capture`의 `zlink::message_t::from_json`, `restore`의 `parse_json`)로 한정하여 Framework의 실행 상태와 게임 상태의 관리 책임을 분리한다.

플레이어의 HP와 위치 같은 게임 사실은 그것을 변경하는 객체 한 곳에서 관리한다. Spot에는 플레이어 ID와 관찰용 복사 값을 보관하고 Actor 참조는 보관하지 않는다. Spot handler는 Actor를 인자로 받으며 다른 플레이어에게는 `actor_client_t`로 플레이어 ID를 지정해 보낸다. Session은 인증과 binding 뒤 packet을 전달하고, 이동과 전투 판정은 해당 상태를 관리하는 `Room`에서 수행하며, `HttpLobby`는 HTTP 요청을 짝짓고 Room을 준비하는 일만 한다. 게임 규칙은 `server/match/domain.hpp`에 두고 Framework callback은 Spot·Actor·Session 파일에 둔다.

### 비동기 실행과 lifecycle

handler는 `task_t` coroutine으로 작성하고 공개 비동기 호출은 템플릿처럼 `co_await`한다. `.result()` 같은 blocking 대기는 [Host execution model](https://docs.zlink.systems/ko/cpp/guide/server/43-execution-model/)이 테스트와 client에만 허용한다. handler에서 대기나 동기 I/O로 실행을 중단하지 않는다. `NodeControl`은 자신의 stdin thread에서, `HttpLobby`는 Room을 준비할 때 자신의 IO thread에서 `await_host`로 대기하며, 둘 다 Spot이나 Actor handler 안에서 실행되지 않는다. SpotWide 실행 모드가 같은 상태의 실행을 하나의 gate로 제한하므로 별도 lock이나 queue로 같은 상태를 다시 보호하지 않는다. await 중에도 turn을 유지할 수 있으므로 같은 Spot에 대한 request처럼 그 turn이 끝나야 실행되는 작업을 기다리지 않으며, 긴 작업은 handler 밖에서 처리한다. 실행 경계는 [Execution model](https://docs.zlink.systems/ko/cpp/guide/server/32-execution-model/)이 설명한다.

타입 등록과 객체별 callback은 각각 정해진 lifecycle 단계에 둔다. factory와 Entry Spot은 node의 `main.cpp`에서 등록하고, handler는 `configure()`에서 등록하고, 참가 수락과 membership 판정은 `on_actor_join`, `on_actor_joined`, `on_leave_actor`, `on_disconnect_actor`에서 처리한다. membership 변경은 `join_spot(...).defer()`와 공개 완료 callback `on_join_completed`를 사용하고 host 종료는 relocation과 runtime 관찰로 확인하여 요청 수락을 완료로 오인하지 않는다. 생성과 초기화의 차이는 [Activation and lifetime](https://docs.zlink.systems/ko/cpp/guide/server/34-activation-lifetime/)이 설명한다.

### 재접속과 실패 처리

reconnect에서는 같은 Actor ID를 사용하고 새 Session을 binding하여 기존 플레이어 상태를 이어서 사용한다. `Session::on_packet`은 `bind_or_get`으로 binding하고 `Room::reconnect`가 새 Session에 현재 `RoomView`를 다시 보낸다. 현재 위치는 Framework가 관리하므로 application route cache를 추가하거나 reconnect 때 플레이어 상태를 초기화하지 않는다. 처리 용량 때문에 호출을 수락하지 못하거나 대기시키는 동작이 backpressure이며, [Backpressure 가이드](https://docs.zlink.systems/ko/cpp/guide/server/33-backpressure/)의 공개 결과를 관찰한다.

Framework 실패는 실제 reply·event·lifecycle 결과를 근거로 조사한다. Internal API·reflection·raw frame·사설 codec·추가 poller·고정 sleep·transport 재시도·deadline 확대·assertion 완화는 실패한 공개 동작을 검증하지 못하므로 해결 방법으로 추가하지 않는다. 코드 주석은 영어로 작성하고 실행·빌드 산출물인 `.logs/`, `build/`, `.zlink/`는 commit에서 제외한다. formatter와 설치 명령은 이 프로젝트 디렉터리에서만 실행하며 위 디렉터리를 제외한 명시적 파일 경로를 지정한다.

## 디버깅

### 실행별 로그

첫 실패 실행을 보존하고 로그를 읽은 뒤 재실행 여부를 결정한다. 간헐 실패도 실제로 관찰한 결함이며 이후 통과로 첫 실패를 제외하지 않는다. `.logs/run-<id>/`에는 `e2e/world.hpp`가 node 시작 시 작성한 실제 설정과 node별 로그가 있다. 선택한 시나리오마다 node를 새로 시작하고 각자의 `.logs/run-<id>/` 디렉터리에 기록한다.

설정 파일은 `lobby.json`, `match-a.json`, `match-b.json`, `session.json`이며 교체한 Match node의 파일 이름에는 `-replacement` 접미사가 붙는다. `e2e/process.hpp`는 진단 stderr를 `<node>.err.log`에, 준비·제어 stdout을 `<node>.out.log`에 기록하며 `<node>`는 설정 파일 이름에서 `.json`을 뺀 것이다. 보조 명령(Compose와 Docker CLI 호출)은 `command-<n>.out.log`와 `command-<n>.err.log`에 기록한다. 로컬 빌드 출력은 `.logs/build.log`에 있다. Docker 빌드와 바깥 정리 로그는 `.logs/docker-<project>/`에 있으며 `build.log`, `redis-start.log`, `redis.log`, `cleanup.log`, 그리고 PowerShell runner의 `containers-stop.log`다.

### 진단 수준

`server/shared/hosting.hpp`의 `configure_host`는 메시지 수신부터 처리 완료까지의 단계를 기록하는 message flow를 다음 설정으로 활성화한다.

```cpp
options.configure_dispatch ().message_flow (fx::message_flow_log_mode_t::normal);
```

`normal`은 receive·dispatch·completion 단계와 오류를 기록한다. 시간 상세가 필요하면 `detailed`로 바꾸며, 크기 기록에는 같은 dispatch 옵션에 `.include_message_sizes (true)`를 이어서 설정한다. 상세 기록에는 비용이 추가되므로 필요한 조사 범위에서 사용한다.

`server/shared/config.hpp`의 `NodeConfig`는 diagnostics JSON key를 정의하지 않으므로 시작 설정은 `configure_host` 호출을 수정하고 다시 빌드·시작하여 적용한다. 실행 중에는 host `app_t`의 `set_message_flow_mode_async`로 바꿀 수 있지만 이 프로젝트의 stdin 제어 명령에는 연결되어 있지 않다. 동기 `set_message_flow_mode`는 blocking 동작이므로 Framework handler와 callback에서는 coroutine 형태를 사용한다. 진단 항목과 설정은 [Monitoring](https://docs.zlink.systems/ko/cpp/guide/server/26-monitoring/)과 [Observability diagnostics](https://docs.zlink.systems/ko/cpp/reference/08-observability-diagnostics/)가 설명한다.

### Message flow 조사

`configure_host`는 Framework 로그 record를 한 줄에 JSON 객체 하나로 stderr에 쓰는 logging sink도 설치하므로 message flow는 node의 `.err.log`에서 확인하며 별도 flow 파일을 만들지 않는다. message flow record는 `"event_id":"zlink.message_flow"`이고 `phase`, `outcome`, `packet`을 가진다. `flow`는 node를 통과하는 메시지를 추적하는 값이고 `corr`는 request와 reply record의 관계를 확인하는 값이다. dispatch 실패는 `"event_id":"zlink.dispatch_error"`, `"outcome":"failed"`, `reason`, `error_type`, `error_message`로 표시된다.

실패 필드와 바로 앞 단계를 함께 읽고 category를 제외하지 않는다. 실행마다 flow ID가 달라지므로 통과·실패 실행은 논리적 작업, Actor ID와 단계 순서로 비교하여 처음 누락되거나 실패한 단계를 찾는다. startup이 message flow 기록 전에 실패했으면 node가 stderr에 쓴 시작 예외 문구를 보존한다.

### 디버거와 정리

로컬 모드에서 runner는 위 설정 파일로 `build/bin/my-game-<role>` process를 시작하므로, 디버거는 시작된 node process에 attach한다. 기본 Docker node는 호스트 환경 변수를 상속하지 않는다. runner는 자신이 만든 process와 Compose project를 정리하며, 정리 오류도 보존하고 다른 작업의 process나 container는 중단하지 않는다. 정리에 실패하면 실행도 종료 코드 1로 실패한다.

## ZLink 결함 보고

### 결함 판단

ZLink 결함은 공개 API를 문서대로 호출했는데 Framework가 문서와 다른 결과를 내는 경우다. 먼저 사용한 API를 위의 공개 가이드·샘플과 대조하여 application 사용 오류인지 확인한다. 사용 오류가 아니면 우회 코드를 추가하지 않고 issue로 보고하며, scenario assertion은 그대로 유지한다. 우회 코드는 결함을 감추고, 수정된 package로 올린 뒤에도 남아 동작을 바꾸기 때문이다.

### 기존 issue 확인

등록 전에 [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue)에서 오류 메시지·예외 타입·API 이름으로 열린 issue와 닫힌 issue를 함께 검색한다. 같은 결함이 있으면 새 issue를 만들지 않고 그 issue에 재현 환경과 로그를 덧붙인다. 닫힌 issue와 같은 결함이면 수정이 포함된 package 버전을 issue에서 확인하고, `bootstrap.cmake`의 `ZLINK_FRAMEWORK_CPP_VERSION`을 올려 다시 실행한다.

### issue 본문

본문은 ZLink 저장소의 issue template 항목을 따른다. 다음 형식을 복사해 채운다.

```markdown
# Issue description
<scenario and the public operation that fails>

# Environment
* zlink version: <ZLINK_FRAMEWORK_CPP_VERSION from bootstrap.cmake>
* OS: <OS, compiler and CMake versions>

# Minimal test code / Steps to reproduce the issue
1. <commands or minimal code using public APIs only>

# What's the actual result? (include assertion message & call stack if applicable)
<first failing node-log excerpt, error text, flow/corr records>

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

제목에는 언어와 실패한 동작을 적는다(예: `framework(cpp): Spot startup fails with empty SpotId`). 등록한 issue 번호는 README의 Known issues에 적어, 수정 package가 나오면 다시 검증할 위치를 남긴다.
