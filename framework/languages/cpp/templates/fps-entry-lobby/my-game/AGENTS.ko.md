# MyGame FPS 개발 가이드

## 프로젝트와 개발

이 템플릿의 도메인을 확장하여 FPS 서버를 만듭니다. C++20 프로젝트는 Session gateway, Match host A·B, Redis Location Store를 실행합니다. Entry Spot은 플레이어를 고정 Matchmaking User Spot으로 보내고, 같은 mode를 요청한 플레이어는 동적 Room User Spot에 들어갑니다. 공개 Framework와 connector package는 `bootstrap.cmake`의 0.30.2에 고정되어 있습니다. 실행·배포·알려진 문제는 [README](README.md)가 소유하고, [zlink.structure.json](zlink.structure.json)은 초기 topology와 시나리오 순서를 설명합니다.

Docker Compose v2와 Linux container를 준비한 뒤 프로젝트 루트에서 실행합니다.

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario matchmaking,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
```

PowerShell 7에서는 다음 명령을 사용합니다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario matchmaking,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

호스트 process 디버깅은 C++20 compiler(GCC 13 이상 또는 Visual Studio 2022 17.4 이상), CMake 3.24 이상과 Ninja를 설치합니다. Windows에서는 Visual Studio 개발자 셸을 사용합니다. `--local` 또는 `-Local`을 지정하고, 로컬 빌드가 있으면 `--no-build` 또는 `-NoBuild`를 추가합니다. 재실행 전에 최초 실패와 assertion을 확인합니다.

## 모듈과 확장 위치

- `server/shared/contracts.hpp`는 client·내부 typed contract, action 한도와 protocol 이름을 정의합니다. `server/match/domain.hpp`는 Vec3, 닫힌 slab ray/AABB 교차, 고정 map, spawn 위치, player 상자와 RoomState를 정의합니다. `server/match/matchmaking.hpp`는 Entry Matchmaking Spot이 사용하는 mode queue와 Room ID sequence 상태를 정의합니다.
- `server/match/domain.hpp`의 `RoomState`가 위치·HP·phase·winner·공유 sequence·이동 예산·발사 cooldown·명중 대상을 소유합니다. 무기·점수·match 규칙은 이 domain을 확장합니다. 운영 시간은 monotonic clock을 사용하고 시험 시계는 domain 생성자에서만 받습니다. snapshot은 process의 절대 시각 대신 남은 시간을 millisecond로 저장합니다.
- `server/match/player.hpp`와 `server/match/spots.hpp`는 Entry·Matchmaking·Room·Player Actor, handler와 relocation adapter를 둡니다. Framework callback을 domain 안으로 넣지 않습니다.
- `server/session/session.hpp`는 연결 callback, Actor binding과 client packet allowlist를 둡니다. `QueueRequest.playerId`는 검증되지 않은 값으로 취급하고 binding 전에 인증합니다. client가 내부 assignment나 결과 packet을 relay하게 두지 않습니다.
- `server/shared/config.hpp`와 `server/shared/hosting.hpp`는 process 설정, hosting, 진단, drain과 shutdown observation을 담당합니다. routing·membership·binding·relocation은 Framework가 소유합니다.
- `e2e/scenarios.hpp`는 matchmaking·hit·wall·input·reconnect·update에서 client가 관찰하는 결과를 검사합니다. `e2e/domain_test.cpp`는 domain 경계 검사이며 시나리오 전에 실행하고 `my-game-domain-test`로도 빌드합니다. `e2e/structure.hpp`는 infrastructure 시작 전에 descriptor와 설정을 검사합니다. `e2e/client.hpp`·`process.hpp`·`world.hpp`는 각각 client, process와 기반 시설의 수명을 관리합니다.

## Domain 소유와 비동기 실행

이동 예산, 발사 간격과 최근접 명중 판정은 RoomState 한 곳에서 결정합니다. Y가 위쪽이며 map 범위와 고정 벽 AABB는 서버 상수입니다. 이동은 발 위치의 선분을 검사합니다. 발 위치 Y는 0을 유지하며 0이 아닌 `delta.y`는 `vertical-move`로 거부합니다. 점프와 중력은 구현하지 않았습니다. 상자 면에 닿아도 교차로 판정하고, 거리 동률이면 벽이 먼저입니다. 허용된 빗나감과 벽에 막힌 발사는 sequence와 cooldown을 사용합니다. 거부된 입력은 상태와 시간을 바꾸지 않습니다. RoomView는 마지막 sequence를 제공하므로 재접속한 client가 다음 값을 이어서 사용합니다.

명중은 현재 서버 상태를 기준으로 계산합니다. 지연 보정, 투사체와 client 예측은 없습니다. handler나 client에 두 번째 판정자를 추가하지 말고 기존 geometry·규칙·typed contract를 확장합니다.

게임 상태는 Spot domain에 두고 공개 Actor·Spot API를 사용합니다. Framework는 handler turn을 직렬화하므로 같은 실행 권한을 위한 lock·queue·generation 검사를 추가하지 않습니다. handler turn에서 infrastructure 완료를 동기 대기하지 않습니다. application handler에서 public 비동기 API를 호출하는 사용은 허용됩니다. Relocation adapter는 게임 상태만 직렬화하며 host 이전·location·route continuity는 Framework가 소유합니다. 명령 수락만으로 완료를 판정하지 말고 relocation 완료와 safe shutdown을 관찰합니다.

## 공개 API 참고 자료

[ZLink 문서](https://docs.zlink.systems/), [C++ server guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/)와 [agent index](https://docs.zlink.systems/llms.txt)를 사용합니다. 현재 문서의 API를 쓰기 전에 고정 package 버전과 대조합니다. Entry·Spot·Actor·Session·Location·relocation은 공개 Framework API와 기본 typed serializer를 사용합니다. private API·raw frame·application routing 우회는 쓰지 않습니다.

고정된 0.30.2 header는 문서의 typed `SessionInfoRequest` reply 예시와 맞지 않습니다. STREAM write/reply는 `zlink::message_t`를 받지만 `fx::message_t::from(T)`는 typed wrapper를 반환하고 `submit()`은 `void`입니다. Reply를 push로 바꾸거나 수동 encoding을 넣지 말고 public 계약 불일치로 기록합니다.

## 진단과 결함

`server/shared/hosting.hpp`는 dispatch 진단을 Normal로 설정합니다. node stderr에 message-flow record가 남습니다. 실패하면 수정이나 재실행 전에 최초 `.logs/run-<id>/`의 설정·stdout·stderr를 보존합니다. 동작·Actor·flow/corr·transition 순서를 통과와 실패 trace에서 대조합니다. 한 번의 격리 실패도 결함으로 취급하며, 뒤의 통과 결과로 제외하지 않습니다. flow record가 남기 전에 startup이 실패하면 startup exception을 보존합니다.

로컬 Spot discovery 진단은 node를 시작하기 전에 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`을 설정합니다. Docker node는 임의의 호스트 환경 변수를 상속하지 않습니다. timeout·고정 대기·assertion을 바꾸어 실패를 감추지 않습니다. runner가 만든 process와 Compose project만 정리하고 정리 오류도 보존합니다.

문서화된 공개 API가 실패하면 package 버전·OS·명령 또는 최소 공개 API 재현·기대 결과·최초 node exception·관련 flow/corr record를 보존합니다. [기존 issue](https://github.com/zlink-systems/zlink/issues)를 확인한 뒤 보고 초안을 개발자에게 보여 주고 승인 후 게시합니다. credential과 비공개 정보는 제거합니다. 확인된 영향 버전은 README의 Known issues에 기록합니다.
