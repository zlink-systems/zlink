# Java TicTacToe sample porting inventory

이 문서는 Java `TicTacToe` 샘플을 `.NET` 구현과 공통 TicTacToe 샘플 문서에 맞춰 대조한
작업 목록이다. `gap`이나 `partial`이 남아 있으면 이 샘플은 완료로 보지 않는다.

## 기준 문서와 구현

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `framework/doc/framework/common/sample/tictactoe/README.ko.md` | 이 inventory와 Java sample source | scenario | done | API 2개, Play 2개, Redis room route, stream session, actor, room Spot, milestone observer 흐름을 검증한다. |
| `.NET: TicTacToe.sln` | `standalone.settings.gradle.kts` | build | done | Shared, Client, Server project를 포함한다. |
| `.NET: README.md` | `README.md` | docs | done | Java 역할 구조와 standalone 실행 방법을 설명한다. |
| `.NET: run_sample.sh` | `run_sample.sh` | runner | done | Redis, API A/B, Play A/B, Client를 실행하고 marker를 확인한다. cleanup은 이 runner가 시작한 PID만 정리한다. |
| `.NET: run_sample.ps1` | `run_sample.ps1` | runner | done | shell runner와 같은 역할 구성을 제공한다. |

## Client

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Client/Program.cs` | `Client/src/main/java/.../client/Program.java` | client-entry | done | API URL과 timeout을 읽고 self-check scenario를 실행한다. |
| `.NET: Client/TicTacToeClientScenario.cs` | `Client/src/main/java/.../client/TicTacToeClientScenario.java` | validation | done | 연속 room의 전역 ID와 framework owner 선택, 가득 찬 room의 join 거절, 진행 중 leave 무시, 종료 후 client leave 기반 destroy, host/guest/observer stream 연결, 인증, join, move, win, milestone push를 검증한다. host와 guest가 자기 join 알림을 받지 않는지도 typed callback으로 직접 계수한다. |
| common: client는 API 응답의 Play endpoint 사용 | `TicTacToeClientScenario.java` | validation | done | client 설정에 Play endpoint를 미리 넣지 않고 API 응답의 endpoint 목록으로 연결한다. |
| common: push 대기는 connector public wait API 사용 | `TicTacToeClientScenario.java` | validation | done | `JoinGameNotify`, `PlayerJoinedNotify`, `GameStateNotify`, `WinMilestoneNotify`를 typed wait path로 기다린다. |
| common: inbound observer는 connect 전에 등록 | `TicTacToeClientScenario.java` | validation | done | host·guest·observer connector 생성 직후 observer를 등록한다. marker는 역할, message kind, packet name, request sequence와 payload byte length를 포함하며 payload 검증이나 push 대기를 대신하지 않는다. |

## Shared Contract

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Shared/Contracts/Messages.cs` | `Shared/src/main/java/.../shared/contracts/*.java` | shared-contract | done | HTTP, channel, stream, actor, Spot payload를 Java record/class로 나누어 둔다. |
| common: join wire suffix | `JoinGameMsg.java`, `JoinGameNotify.java`, `JoinGameFailedNotify.java` | shared-contract | done | Actor에는 `JoinGameMsg`를 one-way send하고 current session은 성공 시 `JoinGameNotify`, 실패 시 `JoinGameFailedNotify`를 push한다. `JoinGameReq`와 `JoinGameRes` alias는 두지 않는다. |
| common: JSON payload | shared contract + framework default codec | codec | done | stream, channel, actor, room Spot payload는 sample 호출부에서 codec을 직접 다루지 않는다. |

## Server

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Server/Program.cs` | `Server/src/main/java/.../server/api/ApiProgram.java`, `server/play/PlayProgram.java` | server-entry | done | API와 Play를 별도 실행 진입점으로 시작하며 각 진입점은 설정 파일 경로만 받는다. |
| `.NET: Server/Api/*` | `Server/src/main/java/.../server/api/*` | api-role | done | `/games` HTTP endpoint를 제공하고 player authentication request handler를 API channel builder에 직접 등록한다. |
| `.NET: Server/Configuration/*` | `Server/src/main/java/.../server/configuration/*` | server-config | done | sample endpoint, Redis endpoint, logging, location store 설정을 모은다. RouteMesh routing id는 runtime allocation을 사용한다. |
| `.NET: Server/Play/PlayServer.cs` | `Server/src/main/java/.../server/play/PlayServer.java` | play-role | done | API A/B endpoint를 ClientServer client에 직접 연결하고 stream server, actor runtime과 Spot route를 구성한다. |
| common: Play domain 경계 | `server/play/domain/tictactoe/*` | domain | done | board, turn, win/draw 판정을 framework 타입 없이 표현한다. |
| common: game creation use case | `server/play/application/gamecreation/TicTacToeGameCreator.java` | application | done | room 생성과 Redis room route 기록을 조율한다. |
| common: ZLink adapter 경계 | `server/play/infrastructure/zlink/*` | framework-adapter | done | session, actor, entry Spot, game Spot callback을 application/domain 호출로 변환한다. |
| common: manual handler registration | `ApiServer`, `PlayServer`, `PlayEntrySpot`, `TicTacToeGame` | framework-configuration | done | channel·session handler는 public builder에, Entry·Room handler는 각 Spot의 public registry에 직접 등록한다. |
| common: observer milestone handler는 entry Spot 책임 | `PlayEntrySpot`, `PlayerWinMilestoneEventHandler` | runtime-flow | done | 별도 public notification Spot 타입을 만들지 않고 entry Spot 안에서 milestone push를 처리한다. |

## Runner 검증

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| Redis room route store | `run_sample.sh`, `SampleLocationStore`, `RedisRoomRouteStore` | external-adapter | done | runner가 실행별 전용 Docker Redis를 만들고 그 endpoint만 사용한다. |
| manual registration gate | `run_sample.sh` | validation | done | Java TicTacToe server source가 `addHandlersFromPackageOf(...)`에 의존하면 역할을 시작하기 전에 실패한다. |
| API A/B와 Play A/B scale-out | `run_sample.sh` | validation | done | runner가 API A/B endpoint를 두 Play에 모두 전달하고, 각 Play가 두 endpoint를 ClientServer client에 직접 연결한다. Object route는 Redis Location Store에서 조회한다. |
| observer milestone | runner grep + client marker | validation | done | observer가 별도 Play stream에 연결하고 `WinMilestoneNotify`의 business payload와 누적 win count를 확인한다. physical owner와 수신 NodeRid는 성공 조건으로 사용하지 않는다. |
| message flow marker | role stdout grep | validation | done | runner가 role별 stdout의 `message flow` marker를 확인한다. |
| final marker | `run_sample.sh` | validation | done | client의 `tictactoe completed` 계열 marker와 `PASS TicTacToe.Java`를 확인한다. |

## 현재 결론

Java `TicTacToe`는 수동 endpoint 연결과 수동 handler 등록을 코드에 명시한다. 다른 Java sample의
automatic discovery 규칙과 섞이지 않도록 `SampleReleaseGateContractTest`가 TicTacToe 예외와 등록 목록을 검증한다.

## 검증

- standalone Gradle 설정으로 `Server:compileJava`와 `Client:compileJava`가 통과했다.
- `SampleReleaseGateContractTest`는 17개 모두 통과했다.
- `run_sample.sh`는 `bash -n`, `run_sample.ps1`은 PowerShell parser 검사를 통과했다.
- 실제 process sample과 전체 sample gate는 이 변경에서 실행하지 않았다.
