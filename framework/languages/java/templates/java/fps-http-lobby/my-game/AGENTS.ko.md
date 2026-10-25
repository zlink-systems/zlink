# MyGame FPS 개발 안내

## 프로젝트와 명령

이 Java 25 프로젝트는 HTTP Lobby, Session gateway, Match host A·B, Redis Location Store로 구성됩니다. Lobby는 `POST /match` 요청을 mode별로 매칭하고, GUID Room ID를 발급한 뒤 동적 Room User Spot의 멤버 명단을 준비합니다. client는 STREAM으로 Session에 연결하고 할당된 Room에 입장합니다. 공개 Framework와 connector 버전은 `gradle/libs.versions.toml`에서 0.30.2로 고정합니다. 실행과 배포 절차는 [README](README.md), 토폴로지와 시나리오 순서는 [zlink.structure.json](zlink.structure.json)이 소유합니다.

Docker Compose v2와 Linux container를 사용하는 명령은 프로젝트 루트에서 실행합니다.

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./gradlew --no-daemon :server:match:test :server:lobby:test
```

PowerShell 7에서도 runner를 실행할 수 있습니다.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

호스트 process를 디버깅하려면 JDK 25를 설치하고 `--local` 또는 `-Local`을 지정합니다. 실행 로그는 `.logs/`에 보존합니다. 실패를 다시 실행하기 전에 최초 실패 로그를 확인합니다.

## 모듈과 소유권

- `server/shared/src/main/java/MyGame/`에 typed 게임 계약, HTTP 요청·응답 계약, 수치와 protocol 이름이 있습니다. `Geometry.java`는 Vec3, 닫힌 slab ray/AABB 교차, 맵 경계, 벽과 스폰을 소유합니다.
- `server/match/src/main/java/MyGame/match/domain/RoomState.java` 한 곳이 위치, HP, phase, winner, 공유 순번, 이동 예산, 발사 간격과 명중 선택을 소유합니다. 운영 시 단조 시계를 사용하고, 도메인 생성자에서만 시험용 시계를 받습니다. `RoomSnapshot`은 process 절대 시각 대신 남은 시간을 저장합니다.
- `server/lobby/src/main/java/MyGame/lobby/application/Matchmaker.java`는 mode 대기열과 매칭을 소유합니다. Room ID 발급 위치는 여기 한 곳이며 RoomPrefix 뒤에 하이픈 없는 UUID를 붙입니다. `RoomProvisioner.java`는 `PrepareRoom`을 보내고 `RoomPrepared`를 받은 뒤 HTTP 할당을 반환합니다.
- `server/match/src/main/java/MyGame/match/domain/RoomRoster.java` 한 곳이 준비된 멤버 명단과 입장 판정을 소유합니다. Room relocation은 `{Game, Roster}`를 함께 저장합니다. Framework callback은 도메인에 넣지 않습니다.
- `server/session/src/main/java/MyGame/session/GameSession.java`는 client packet 허용 목록과 Actor binding을 소유합니다. 정상 client의 첫 packet은 JoinRoomRequest입니다. 배포 전에는 주장된 player ID를 인증합니다. preparation, readiness, assignment, result packet을 중계하지 않습니다.
- `e2e/src/main/java/MyGame/e2e/Scenarios.java`는 lobby, hit, wall, input, reconnect, update 관찰을 검증합니다. `server/match/src/test/java/MyGame/match/domain/RoomStateTest.java`는 게임 도메인 경계 45개를 확인하고, `server/lobby/src/test/java/MyGame/lobby/application/LobbyDomainTest.java`는 로비 경계 19개를 확인합니다.

HTTP handler는 공개 Java `ZLinkRouteClient`를 주입받아 `requestToSpot`으로 Room을 준비합니다. `ZLinkSpotOutbound`는 Spot callback 안에서만 사용합니다.

## 게임 규칙

Y축이 위이며 맵 경계는 X/Z=0..100, Y=0..10입니다. 발 위치는 Y=0이고 플레이어 AABB는 폭 1, 높이 2, 깊이 1이며 눈 높이는 1.6입니다. 수직 이동은 `vertical-move`로 거부하며 점프와 중력은 구현하지 않습니다. 이동은 발 위치 선분을 닫힌 벽 상자와 비교하며 끝점 접촉도 교차로 판정합니다.

이동 최대 step은 2, 이동 예산은 초당 10 단위이며 마지막 수락 이동 이후 최대 2 단위까지 누적됩니다. 발사는 유한한 0이 아닌 방향을 authoritative 눈 위치에서 정규화합니다. 사거리는 80, 피해는 25, 간격은 250ms입니다. 가장 가까운 벽 또는 생존 플레이어 AABB를 선택하고, 같은 거리면 벽을 선택합니다. 명중하지 않은 발사와 벽에 막힌 발사도 순번과 cooldown을 소비합니다. 거부는 위치, HP, 순번, 시간을 바꾸지 않습니다. client는 서버가 준 `RetryAfterMs` 뒤 같은 입력을 다시 보낼 수 있습니다.

`POST /match`에서 mode를 생략하면 `duel`입니다. Matchmaker가 Room ID를 발급하고, Lobby는 RoomPrepared 응답을 받은 뒤에만 HTTP 응답을 반환합니다. 같은 명단의 준비 요청은 멱등이고 다른 명단은 거부합니다. Room snapshot은 게임 상태와 명단을 함께 보존해 relocation 뒤에도 입장 판정을 유지합니다.

명중 판정은 현재 서버 상태를 사용합니다. 지연 보정, 투사체 시뮬레이션, client 예측은 없습니다. 게임 규칙을 확장할 때 `RoomState`, `RoomRoster`, `Geometry`, typed contract를 수정하고 기존 소유 위치에 판정을 둡니다. 메시지는 Framework의 기본 typed serializer를 사용합니다. Actor와 Spot은 public API로 연결하며 라우팅, binding, membership, completion, relocation 결정은 Framework 소유로 둡니다.

## 진단과 결함 보고

실패한 실행의 `.logs/run-<id>/` 설정과 node 로그를 첫 재현부터 보존합니다. message-flow의 operation, Actor, flow/corr, transition 순서를 비교합니다. 한 번의 격리 실패도 결함입니다. timeout을 늘리거나 고정 대기를 추가하거나 시나리오 assertion을 낮춰 실패를 숨기지 않습니다.

문서화된 public API가 계약과 다르게 동작하면 package 버전, OS, 명령 또는 public API 최소 재현, 기대 결과, 최초 node 예외와 관련 flow 기록을 남깁니다. 보고 전에 기존 ZLink issue를 확인합니다. 자격 증명과 개인 정보는 제거합니다.
