# MyGame 보드 테이블

## 포그라운드 서버와 e2e

로컬 C++ 도구를 설치한 뒤 서버를 시작합니다. `Server ready`가 표시되면 같은 템플릿 디렉터리의 두 번째 터미널에서 e2e를 실행합니다.

| 셸 | 서버 실행 | e2e 실행 |
| --- | --- | --- |
| Bash | `./run-server.sh` | `./run-e2e.sh` |
| Windows | `.\run-server.cmd` | `.\run-e2e.cmd` |
| PowerShell 7 | `pwsh -NoProfile -File ./run-server.ps1` | `pwsh -NoProfile -File ./run-e2e.ps1` |

e2e는 실행 중인 서버에 자동으로 연결합니다. 서버를 Ctrl+C로 종료한 뒤 e2e를 실행하면 기본값으로 Docker 격리 환경을 시작합니다. 로컬 격리 실행에는 `--local` 또는 `-Local`을 사용합니다. 로컬·포그라운드 실행에 `ZLINK_REDIS_TEST_ENDPOINT=host:port`를 설정하면 기존 Redis 인스턴스를 사용합니다. 실행마다 고유한 key prefix를 사용합니다.

시나리오는 `table,reconnect,match,match-cancel,persist,update` 순서로 실행합니다. `--scenario match,match-cancel,persist` 또는 `-Scenario match,match-cancel,persist`로 선택합니다. `update`는 play node를 교체하므로 격리 실행에서 수행합니다. 화면에 표시되는 `.logs/run-<id>/`에서 노드별 `<node>.out.log`로 준비·제어 내역을, `<node>.err.log`로 Framework message flow를 확인합니다.

## 매치메이킹과 Actor DB

Session이 출력한 WebSocket URL로 연결합니다. 테이블 ID를 지정해 입장하려면 `JoinTableRequest`를, 대기열에 등록하려면 `FindMatch`를 보냅니다. Matchmaker node의 `Pool` Instance Spot은 play node의 `RoomProvisioner` Instance Spot으로 `PrepareRoom`을 보냅니다. `RoomProvisioner`가 Table을 만들고 `RoomPrepared`로 응답합니다. 두 Actor가 입장하면 Table이 매치를 확정합니다.

Player Actor는 `LoadPlayer` 한 번으로 잔액을 읽고 입장 요청에 담습니다. Table은 라운드가 끝나면 한 번의 `CommitRound`로 잔액을 저장하고 확인을 받은 뒤 결과를 보냅니다. API node는 Redis에 라운드 식별자와 잔액을 원자적으로 저장합니다. `persist`는 새 API node·play node·Session이 같은 플레이어 기록을 사용하고, 새 Actor가 정산 잔액을 읽는지 확인합니다. `api.json`에서 API listener 주소를 설정합니다. Play node는 공유 Location Store에서 채널을 찾습니다. API node를 교체할 때 플레이어 기록의 key prefix를 유지합니다. hand 번호를 처음부터 부여하는 Table에는 새 ID를 사용합니다.

## 메시지 계약과 연결 종료

1. 공유 DTO에 null 허용과 JSON 표현을 선언합니다.
   - null을 허용하는 DTO 필드는 `std::optional<T>`로 선언합니다. 필드 생략을 허용하려면 `dto_traits`에서 별도로 `zlink::json::defaulted`를 지정합니다.
   - 64비트 정수는 JSON에서 10진 문자열로 표현합니다. 브라우저 클라이언트도 `"handNumber": "42"`처럼 문자열을 보냅니다. [메시지 모델](https://docs.zlink.systems/cpp/guide/server/20-channel-messaging/#12-message-value-rules)

시나리오 하나만 실행하려면 `./run-e2e.sh --scenario reconnect`를 사용합니다. 마지막 request의 응답을 받은 뒤 연결을 닫습니다. send 직후 연결을 닫으면 서버가 메시지를 처리하지 못할 수 있습니다.

[영어 README](README.md)에서 빌드, e2e 실행, 배포 절차를 확인합니다.

이전에 배정된 좌석으로 재접속할 때는 `JoinTableRequest`에 `reconnect: true`를 보냅니다. Actor가 속한 Table에서 요청을 처리합니다. 소속 Table이 없으면 Entry가 `ActionRejected`(`not-seated`)로 응답합니다. 이전 테이블 ID를 버리고 테이블 선택으로 돌아갑니다. 첫 입장에서는 `reconnect`를 생략하거나 `false`를 보냅니다.

## 알려진 문제

Framework 0.34.1은 play node가 자동 발견으로 플레이어 기록 채널을 등록한 경우 `update`에서 `ManualTopologyUnsupported`를 반환합니다. 시나리오는 이 relocation 실패를 보고합니다.

drain 확인에 약 30초가 걸리므로 대기하는 동안 진행 중인 판의 턴 시간이 끝날 수 있습니다. 턴 시계가 있는 게임의 update 시나리오에서는 판을 끝낸 뒤 옮깁니다. 남은 시간이 이전되는지는 시계를 제어하는 도메인 검사에서 확인합니다.
