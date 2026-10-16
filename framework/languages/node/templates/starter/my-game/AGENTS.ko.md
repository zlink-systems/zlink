# MyGame 개발 지침

이 Node.js 22 셋업은 Session node, Redis hosting, 진단, stdin 제어와 연결 e2e 실행기를 제공한다. 게임 상태와 게임 패킷은 포함하지 않는다. 빌드·실행·배포 명령은 [README](README.md)가 소유하며, [영어 지침](AGENTS.md)도 같은 구성을 설명한다.

## Node 책임

| 모듈                        | 책임과 확장 위치                                                      |
| --------------------------- | --------------------------------------------------------------------- |
| `server/session/main.ts`    | STREAM과 mesh listener 등록, application 객체 도입 시 등록 추가       |
| `server/session/session.ts` | 연결 callback, 인증과 application 패킷 처리 추가                      |
| `server/shared/`            | 설정, hosting, 단일 `Control` 마커 정의                               |
| `e2e/client.mts`            | 관찰 가능한 시나리오, `Scenarios`과 구조 선언에 시나리오 ID 함께 추가 |
| `e2e/runner.ts`             | node 기동, 로그 보존과 소유 자원 정리                                 |
| `e2e/structure.ts`          | 기동 전 실제 설정과 등록 선언 검사                                    |

`zlink.structure.json`은 초기 구성을 기술하며 runtime 배치를 정하지 않는다. 이 셋업은 application channel과 Spot을 선언하지 않는다. 이를 추가할 때 검사기도 등록 상수의 이름·타입과 대조하도록 확장한다.

## 샘플을 통한 개발

[ZLink Start](https://zlink.systems/start)에서 필요한 구조를 받아 실행한 뒤 그 기능을 추가한다. 샘플은 구조 9종을 서버 언어 5개로 제공한다. 샘플은 별도로 유지하고 필요한 등록·application 상태·검증만 가져온다.

다음 표에서 게임에 필요한 구성을 고른다.

| 구조                     | 선택 기준                                             |
| ------------------------ | ----------------------------------------------------- |
| `board-tables`           | 플레이어 Actor가 같은 테이블 Spot에 입장하는 보드게임 |
| `match-entry-lobby`      | STREAM 첫 입장, 매칭 대기열과 동적 대전 방            |
| `match-http-lobby`       | HTTP 매칭 뒤 STREAM으로 대전 방에 입장                |
| `morpg-lobby-instances`  | 로비에서 파티를 구성한 뒤 전용 던전에 입장            |
| `morpg-channel-town`     | 마을 채널에서 파티를 구성한 뒤 전용 던전에 입장       |
| `morpg-fields-instances` | 여러 node의 공용 필드, 경계 표시와 파티 던전          |
| `mmo-world-channels`     | 월드별 데이터 격리와 월드 안의 필드 채널 변경         |
| `mmo-megaserver`         | 같은 지역의 정원이 차면 지역 instance를 추가 생성     |
| `mmo-seamless`           | Zone 경계를 넘는 연속 이동과 경계 구독                |

다음 기능 표의 경로는 ZLink 저장소의 `framework/languages/node/templates/`를 기준으로 한다. 내려받은 샘플에서는 `<structure>/my-game/` 뒤의 경로를 사용한다. 같은 행의 짧은 파일 경로는 주 파일의 디렉터리 또는 명시된 프로젝트 루트를 기준으로 한다.

| 기능                        | 참고 파일과 가져올 내용                                                                                                                          |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| 공유 Spot                   | `board-tables/my-game/server/table/infrastructure/table.ts`: 테이블 상태와 admission, `board-tables/my-game/server/table/main.ts`의 등록         |
| 테이블 Actor                | `board-tables/my-game/server/table/infrastructure/player.ts`: 최소 플레이어 Actor, join 완료와 factory                                           |
| Entry·첫 입장               | `morpg-channel-town/my-game/server/shared/entry.ts`: 첫 패킷 handler와 Town으로의 deferred join                                                  |
| Session binding             | `board-tables/my-game/server/session/session.ts`: Actor 생성·조회, binding과 원래 typed 패킷 relay                                               |
| Host relocation             | `board-tables/my-game/server/table/infrastructure/table.ts`: relocation adapter의 capture/restore, `e2e/client.mts`의 update 검증                |
| 동적 던전                   | `morpg-lobby-instances/my-game/server/lobby/spots.ts`: getOrCreate와 준비, `server/instance/dungeon.ts`와 `server/instance/main.ts`의 구현·등록  |
| 월드·채널 선택              | `mmo-world-channels/my-game/server/channel/handlers.ts`: ChangeChannel handler의 deferred membership 변경, `server/accounts/main.ts`의 월드 목록 |
| Channel pub/sub·심리스 이동 | `mmo-seamless/my-game/server/zone/zone.ts`: 경계 구독·발행, `handlers.ts`의 전달과 `player.ts`의 이동                                            |
| HTTP 로비                   | `match-http-lobby/my-game/server/lobby/http.ts`: HTTP endpoint, `matchmaker.ts`의 매칭과 `http.ts`의 방 생성                                     |
| Typed 메시지                | `board-tables/my-game/server/shared/messages.ts`: 공유 패킷 class                                                                                |

`mmo-world-channels`의 채널은 플레이어가 선택하는 월드 안의 게임 공간이다. 메시지 구독 channel과는 구분하며, 이 샘플에는 pub/sub 구독이 없다. 경계 메시지 구독은 `mmo-seamless`의 실제 등록·handler를 참고한다.

기능마다 설정·등록, `zlink.structure.json`, 검사기와 e2e를 함께 확장한다. 게임 규칙이 생기면 Domain/Application에 두고 Framework callback과 transport adapter는 Infrastructure/ZLink에 둔다. 현재 셋업에는 빈 계층을 만들지 않는다.

Typed 패킷을 추가할 때 `board-tables/my-game/scripts/generate-framework-json-schemas.mjs`, build 명령과 hosting의 schema import를 함께 가져온다. Typed payload는 샘플 handler의 패킷 이름→class 복원 방식(#1731)을 따른다. 현재 셋업에는 typed handler가 없다.

## 공개 API 경계

[공개 문서](https://docs.zlink.systems/), [Node.js 서버 가이드](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), [agent index](https://docs.zlink.systems/llms.txt)를 참고한다. 온라인 문서는 더 최신 릴리스를 설명할 수 있으므로 `package.json`의 버전에서 지원하는 API인지 확인한다.

공개 API와 Framework 기본 typed JSON serializer를 사용한다. Framework 실패를 보완하기 위한 reflection, internal API, raw frame, 수동 codec, sleep, 재시도, deadline 확대와 assertion 완화를 추가하지 않는다.

각 사실은 상태 소유자 한 곳에서 관리한다. 게임 기능을 추가하면 Actor handler의 응답은 그 Actor의 bound Session으로 보낸다. 그 밖의 push는 player ID로 Actor에게 보내고 수신 Actor handler가 bound Session으로 전달한다. host relocation의 target은 `onJoinedActor`를 다시 호출하지 않으므로 Spot은 나중에 전달할 Actor 참조를 저장하지 않는다. 라우팅·lease·execution gate·membership은 Framework가 소유하며 relocation adapter는 application 상태만 보존한다.

## 변경과 검증

현재 디렉터리에서 README의 명령을 실행한다. debugger를 연결할 때는 `--local` 또는 `-Local`을 사용하고 기동한 node process에 연결한다. 현재 smoke 검사는 `connect` 시나리오가 담당하며 게임 시나리오는 이를 유지하면서 추가한다. 실제 수신 결과 assertion을 유지하고 성공뿐 아니라 실패 종료 코드도 확인한다.

OS가 배정한 포트, 고유 Redis key prefix와 Compose project를 사용한다. 실행기가 만든 자원만 정리한다. 정리 오류와 간헐 실패도 실패이며 다음 실행 전에 첫 로그를 보존한다.

runner가 실행 결과와 connector 정리 실패를 소유한다. 시나리오 예외를 먼저, 정리 오류를 다음에 보고하며 어느 쪽이든 실패하면 exit 1을 반환한다. 개별 정리 오류가 있어도 node·container·Redis 정리를 계속한다.

## 디버깅 설정

`runHost`는 dispatch diagnostics를 `Normal`로 설정하고 Framework message flow를 node stderr에 기록한다. stdout과 stderr는 `.logs/run-<id>/`의 `session-0.out.log`와 `session-0.err.log`에 따로 보존한다. flow listener를 중복 추가하지 않는다.

첫 예외와 직전 flow/correlation 기록을 읽는다. 연결만 검사하는 실행은 게임 패킷을 dispatch하지 않으므로 application message-flow 이벤트가 없을 수 있다. dispatch 이전 기동 실패도 예외와 stack을 보존한다.

로컬 보조 trace는 실행 전에 부모 process 환경에서 설정한다.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_RELOCATION = "1"
```

Bash는 `export ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`을 사용한다. Docker node는 host 환경 변수를 상속하지 않으므로 필요한 진단 값은 node 생성 인자에 명시한다. 이 버전별 진단 switch를 라우팅 API로 사용하지 않는다.

## 결함 보고

먼저 공개 가이드와 동작하는 샘플로 API 사용 오류인지 확인한다. pin 버전, OS, 재현 명령, 첫 node 실패와 flow/correlation 기록을 보존한다. 기존 [ZLink 이슈](https://github.com/zlink-systems/zlink/issues)를 확인하고 같은 결함 번호를 README Known issues에 적는다. coding agent가 새 공개 이슈를 준비하면 게시 전에 개발자에게 본문을 제시한다. 첨부 로그에서 자격 증명과 개인 정보를 제거한다.
