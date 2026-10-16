# MyGame 개발 지침

이 Java 25 셋업은 Session node, Redis hosting, 진단, stdin 제어와 연결 e2e 실행기를 제공한다. 게임 상태와 게임 패킷은 포함하지 않는다. 빌드·실행·배포 명령은 [README](README.md)가 소유하며, [영어 지침](AGENTS.md)도 같은 구성을 설명한다.

## Node 책임

| 모듈 | 책임과 확장 위치 |
| --- | --- |
| `server/session/src/main/java/MyGame/session/Program.java` | STREAM과 mesh listener 등록, application 객체 도입 시 등록 추가 |
| `server/session/src/main/java/MyGame/session/PlayerSession.java` | 연결 callback, 인증과 application 패킷 처리 추가 |
| `server/shared/` | 설정, hosting, 단일 `Control` 마커 정의 |
| `e2e/src/main/java/MyGame/e2e/Scenarios.java` | 관찰 가능한 시나리오, `Scenario`와 구조 선언에 시나리오 ID 함께 추가 |
| `e2e/src/main/java/MyGame/e2e/Nodes.java` | node 기동, 로그 보존과 소유 자원 정리, cleanup 실패는 Program이 시나리오 실패 다음에 보고 |
| `e2e/src/main/java/MyGame/e2e/Structure.java` | 기동 전 실제 설정과 등록 선언 검사 |

`zlink.structure.json`은 초기 구성을 기술하며 runtime 배치를 정하지 않는다. 이 셋업은 application channel과 Spot을 선언하지 않는다. 이를 추가할 때 검사기도 등록 상수의 이름·타입과 대조하도록 확장한다.

## 샘플을 통한 개발

[ZLink Start](https://zlink.systems/start)에서 필요한 구조를 받아 실행한 뒤 그 기능을 추가한다. 샘플은 구조 9종을 서버 언어 5개로 제공한다. 샘플은 별도로 유지하고 필요한 등록·application 상태·검증만 가져온다.

다음 표에서 게임에 필요한 구성을 고른다.

| 구조 | 선택 기준 |
| --- | --- |
| `board-tables` | 플레이어 Actor가 같은 테이블 Spot에 입장하는 보드게임 |
| `match-entry-lobby` | STREAM 첫 입장, 매칭 대기열과 동적 대전 방 |
| `match-http-lobby` | HTTP 매칭 뒤 STREAM으로 대전 방에 입장 |
| `morpg-lobby-instances` | 로비에서 파티를 구성한 뒤 전용 던전에 입장 |
| `morpg-channel-town` | 마을 채널에서 파티를 구성한 뒤 전용 던전에 입장 |
| `morpg-fields-instances` | 여러 node의 공용 필드, 경계 표시와 파티 던전 |
| `mmo-world-channels` | 월드별 데이터 격리와 월드 안의 필드 채널 변경 |
| `mmo-megaserver` | 같은 지역의 정원이 차면 지역 instance를 추가 생성 |
| `mmo-seamless` | Zone 경계를 넘는 연속 이동과 경계 구독 |

다음 기능 표의 경로는 ZLink 저장소의 `framework/languages/java/templates/java/`를 기준으로 한다. 내려받은 샘플에서는 `<structure>/my-game/` 뒤의 경로를 사용한다. 같은 행의 짧은 파일 경로는 주 파일의 디렉터리 또는 명시된 모듈 루트를 기준으로 한다.

| 기능 | 참고 파일과 가져올 내용 |
| --- | --- |
| 공유 Spot | `board-tables/my-game/server/table/src/main/java/MyGame/table/Table.java`: 테이블 상태와 admission, `Program.java`의 등록 |
| 테이블 Actor | `board-tables/my-game/server/table/src/main/java/MyGame/table/Player.java`: 최소 플레이어 Actor와 join 완료, `PlayerFactory.java`와 함께 확인 |
| Entry·첫 입장 | `morpg-channel-town/my-game/server/shared/src/main/java/MyGame/admission/EntryActions.java`: 첫 패킷 handler와 Town으로의 deferred join, `Entry.java`와 함께 확인 |
| Session binding | `board-tables/my-game/server/session/src/main/java/MyGame/session/GameSession.java`: Actor 생성·조회, binding과 원래 typed 패킷 relay |
| Host relocation | `board-tables/my-game/server/table/src/main/java/MyGame/table/TableTransfer.java`: application 상태의 capture/restore, `PlayerTransfer.java`와 `e2e/src/main/java/MyGame/e2e/Scenarios.java`의 update 검증 |
| 동적 던전 | `morpg-lobby-instances/my-game/server/lobby/src/main/java/MyGame/lobby/Party.java`: getOrCreate와 준비, `server/instance/src/main/java/MyGame/instance/Dungeon.java`와 `Program.java`의 구현·등록 |
| 월드·채널 선택 | `mmo-world-channels/my-game/server/channel/src/main/java/MyGame/channel/Actions.java`: `change`의 deferred Spot join, `server/accounts/src/main/java/MyGame/accounts/Program.java`의 월드 목록 |
| Channel pub/sub·심리스 이동 | `mmo-seamless/my-game/server/zone/src/main/java/MyGame/border/BorderRoutes.java`: 경계 구독 handler, `MyGame/zone/Zone.java`의 발행과 `MyGame/zone/Actions.java`의 이동 |
| HTTP 로비 | `match-http-lobby/my-game/server/lobby/src/main/java/MyGame/lobby/infrastructure/HttpLobby.java`: HTTP endpoint, `application/Matchmaker.java`와 `infrastructure/ZLinkRooms.java`의 매칭·방 생성 |
| Typed 메시지 | `board-tables/my-game/server/shared/src/main/java/MyGame/JoinTableRequest.java`: 공유 패킷 record, 나머지 record는 같은 디렉터리 |

`mmo-world-channels`의 채널은 플레이어가 선택하는 월드 안의 게임 공간이다. 메시지 구독 channel과는 구분하며, 이 샘플에는 pub/sub 구독이 없다. 경계 메시지 구독은 `mmo-seamless`의 실제 등록·handler를 참고한다.

기능마다 설정·등록, `zlink.structure.json`, 검사기와 e2e를 함께 확장한다. 게임 규칙은 이를 호출하는 Framework callback과 transport adapter에서 분리한 application 모듈에 둔다. 현재 셋업에는 빈 계층을 만들지 않는다.

## 공개 API 경계

[공개 문서](https://docs.zlink.systems/), [Java 서버 가이드](https://docs.zlink.systems/java/guide/server/), [API reference](https://docs.zlink.systems/java/reference/), [agent index](https://docs.zlink.systems/llms.txt)를 참고한다. 온라인 문서는 더 최신 릴리스를 설명할 수 있으므로 `build.gradle.kts`의 버전에서 지원하는 API인지 확인한다.

공개 API와 Framework 기본 typed JSON serializer를 사용한다. Framework 실패를 보완하기 위한 reflection, internal API, raw frame, 수동 codec, sleep, 재시도, deadline 확대와 assertion 완화를 추가하지 않는다.

사실 하나에는 상태 소유자 하나를 둔다. 게임 기능을 추가하면 Actor handler 안의 응답은 그 Actor에 bind된 Session으로 보내고, 그 밖의 push는 player ID로 Actor에게 보내 받은 Actor handler가 bound Session으로 넘긴다. host relocation은 target에서 `onJoinedActor`를 다시 호출하지 않으므로 Spot은 나중에 전달하려고 Actor 참조를 보관하지 않는다. routing, lease, execution gate, membership은 Framework가 소유하며 relocation adapter는 application 상태만 capture한다.

## 변경과 검증

README의 명령을 이 디렉터리에서 실행한다. debugger를 붙일 때는 `--local` 또는 `-Local`을 사용하고 실행 로그 디렉터리의 node 로그를 확인한다. `connect` 시나리오가 현재의 smoke 검사를 소유하므로 게임 시나리오는 이를 대체하지 않고 추가한다. 받은 결과에 대한 assertion을 유지하고 성공뿐 아니라 실패 종료 코드도 검사한다.

OS가 배정한 포트, 실행마다 고유한 Redis key prefix와 Compose project를 사용한다. 그 실행이 만든 자원만 정리한다. cleanup 오류와 간헐 실패도 실패이며, 다시 실행하기 전에 첫 로그를 보존한다.

`Program`은 시나리오 실패를 먼저 출력하고, 이어서 `Nodes.close`가 모은 cleanup 실패를 출력한다. 어느 쪽이든 종료 코드는 1이다.

## 진단 설정

`Hosting.configure`가 dispatch 진단을 `Normal`로 켜고 Framework message flow를 node stderr에 남긴다. node stdout과 stderr는 `.logs/run-<id>/session-<id>/` 아래 `session-stdout.log`와 `session-stderr.log`로 따로 저장된다. 두 번째 flow listener를 추가하지 않는다.

첫 예외와 그 앞의 flow/correlation 기록을 읽는다. 연결만 수행하는 실행은 게임 패킷을 dispatch하지 않으므로 application message-flow 이벤트가 없을 수 있다. dispatch 이전의 기동 실패는 예외와 stack을 그대로 보존해야 한다.

e2e runner의 STREAM connector를 보조로 추적하려면 `--local` 실행 전에 부모 프로세스 환경에 `ZLINK_JAVA_STREAM_TRACE=1`을 설정한다. Docker runner와 node는 호스트 환경 변수를 물려받지 않으므로 필요하면 Compose service나 node 생성에 진단 환경 값을 명시한다. 이 버전별 스위치는 진단용이며 routing에 쓰지 않는다.

## 결함 보고

먼저 공개 API 사용을 가이드와 동작하는 샘플에 대조한다. pin한 버전, OS, 재현 명령, 첫 node 실패와 flow/correlation 기록을 보존한다. 기존 [ZLink 이슈](https://github.com/zlink-systems/zlink/issues)를 확인하고 일치하는 이슈 번호를 README Known issues에 기록한다. 코딩 에이전트가 새 공개 이슈를 준비하면 게시 전에 개발자에게 본문을 보여 준다. 첨부 로그에서 인증 정보와 개인정보를 제거한다.
