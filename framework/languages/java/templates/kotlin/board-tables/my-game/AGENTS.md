# MyGame agent guidelines

## Node 책임

| Module | 책임 |
| --- | --- |
| `server/shared` | typed 계약, 게임 상수, node 설정, 공개 bootstrap과 제어 프로토콜 |
| `server/session` | STREAM 종단, Player GetOrCreate·bind, relay |
| `server/table` | Entry Spot, Table User Spot, 좌석·게임 상태, relocation adapter |
| `e2e` | 공개 connector client, 실제 응답 판정, process·Compose 수명, 구조 검사 |

## 확장 지점

게임 규칙은 `Table.kt`, 메시지는 `Contracts.kt`, 공개 등록은 각 node의 `Program.kt`에서 확장합니다. 설정의 Id와 `zlink.structure.json`을 함께 확인합니다. `Names`와 `Rules`의 상수를 재사용합니다. Initializr는 `my-game`과 `MyGame`을 치환하므로 추가 프로젝트 이름 형태를 도입하지 않습니다.

## 공개 API 경계

공개 Framework·binding package만 사용합니다. 기본 typed JSON serializer를 유지합니다. Player Actor에 Table 상태를 복제하거나 relocation adapter에 Framework route·lease·membership·queue를 넣지 않습니다. SPOT_WIDE gate가 게임 상태를 직렬화합니다. 별도 lock·queue·retry·sleep으로 runtime 결함을 보상하지 않습니다.

`SessionInfo`는 sessionId와 bind된 Actor의 actorId·objectGeneration을 소유하고, `TableView`는 테이블 상태만 전달합니다. 요청 handler의 응답은 요청 Actor의 bound session으로 직접 보내며, 나머지 상태 push는 공개 Actor send API를 사용합니다.

## 검증

`./run-e2e.sh` 또는 `pwsh -NoProfile -File ./run-e2e.ps1`을 실행합니다. 기본 시나리오는 `table`, `reconnect`, `update`입니다. 응답과 runtime observation으로 판단하며 고정 대기를 추가하지 않습니다. 구조 검사는 모든 실행의 시작 단계에서 실제 파일을 검증합니다. 나쁜 구조 입력이 거부되는지는 개발 중 임시 사본에서 확인하고 자기 시험 코드는 템플릿에 남기지 않습니다.

실패 로그와 첫 message flow 오류를 보존합니다. 간헐 실패를 재실행 성공으로 덮지 않습니다. 정리는 자신이 생성한 process·Compose project에 한정합니다. 공개 package 결함은 재현·첫 실패 로그·관련 소스 위치를 보고하고 템플릿에서 우회하지 않습니다. 코드 주석은 영어, 기술 보고는 격식 있는 한국어로 작성합니다.
