# MyGame 개발 규칙

이 프로젝트는 공개 NuGet package를 사용하는 보드 테이블 서버 템플릿이다. 실행 절차는 README.md를 따른다. 코드 주석은 영어로 작성한다.

## Node 책임과 확장 위치

| 프로젝트 | 책임 | 확장 위치 |
| --- | --- | --- |
| `MyGame.Session` | STREAM 종단, Player Actor GetOrCreate·bind, relay | bind 이전 인증 |
| `MyGame.Table` | Entry Spot, 동적 Table User Spot, Player Actor 호스팅 | 입장·게임 규칙·결산 |
| `MyGame.Shared` | 메시지·상수·node 설정·hosting·제어 프로토콜 | 공유 계약 |
| `MyGame.E2E` | 격리 실행·공개 client·관측 결과 판정 | 게임 시나리오 |

Table Spot이 좌석·칩·덱·카드·차례·판돈을 소유한다. Player Actor는 identity와 bound session만 담당한다. 전송 대상은 Table 좌석의 playerId만 사용하며 Actor 참조 사본은 보관하지 않는다. route·lease·membership·queue·execution gate는 Framework가 소유한다.

## 공개 API 경계

- 공개 NuGet package와 템플릿 내부 프로젝트 참조만 사용한다.
- 메시지는 typed message와 기본 JSON serializer를 사용한다. relocation adapter는 application 상태만 직렬화한다.
- Entry Spot은 공개 `GetOrCreate`와 `JoinSpot(...).Defer()`를 사용한다. 요청 handler는 전달받은 Actor의 `Context.BoundSession.Send`로 요청자에게 답한다. 그 밖의 push는 좌석의 playerId로 `IZLinkActorClient.SendToActor`를 호출하고, 공개 Spot-actor 전달 handler가 실제 Actor의 bound session으로 전송한다.
- 재접속은 동일 Actor를 새 Session에 bind한다. 게임 상태를 초기화하거나 application route cache를 만들지 않는다.
- drain은 공개 host relocation과 `SafeToShutdown` 관찰을 사용한다.
- internal API, reflection, raw frame, 수동 codec, poller, sleep, 재시도, deadline 확대 또는 assertion 완화로 Framework 결함을 우회하지 않는다.

## 검증

구성 변경 시 `zlink.structure.json`과 설정을 함께 갱신한다. 구조 검사와 table·reconnect·update 시나리오를 실행한다. 카드 비공개·칩 보존·Actor generation·연결 연속성 검증을 유지한다. 구조 검사에는 self-test를 추가하지 않으며 개발 중 임시 변형 파일로 잘못된 입력이 거부되는지 확인한다.

Windows와 WSL에서 빌드와 E2E를 확인한다. 실패는 첫 실행의 `.logs/`를 읽고 분류한다. 간헐 실패는 한 번 발생해도 결함으로 보고한다. Runner가 소유한 Compose project와 process만 정리한다. `.logs/`, `bin/`, `obj/`, license는 commit하지 않는다.

Node stderr의 message flow는 dispatch diagnostics `Normal` logger로 기록한다. 별도 ActivityListener를 추가하지 않는다.
