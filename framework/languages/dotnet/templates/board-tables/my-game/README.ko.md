# MyGame 보드 테이블

## 메시지 계약과 연결 종료

1. 공유 DTO에 null 허용과 JSON 표현을 선언합니다.
   - null을 허용하는 필드는 `string?`과 같은 nullable 참조 타입이나 `long?`과 같은 nullable 값 타입으로 선언합니다. 필드 생략을 허용하려면 생성자 매개변수에 기본값을 지정합니다.
   - 64비트 정수는 JSON에서 10진 문자열로 표현합니다. 브라우저 클라이언트도 `"handNumber": "42"`처럼 문자열을 보냅니다. [메시지 모델](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/#12-message-value-rules)

시나리오 하나만 실행하려면 `./run-e2e.sh --scenario reconnect`를 사용합니다. 마지막 request의 응답을 받은 뒤 연결을 닫습니다. send 직후 연결을 닫으면 서버가 메시지를 처리하지 못할 수 있습니다.

[영어 README](README.md)에서 빌드, e2e 실행, 배포 절차를 확인합니다.

이전에 배정된 좌석으로 재접속할 때는 `JoinTableRequest`에 `reconnect: true`를 보냅니다. Actor가 속한 Table에서 요청을 처리합니다. 소속 Table이 없으면 Entry가 `ActionRejected`(`not-seated`)로 응답합니다. 이전 테이블 ID를 버리고 테이블 선택으로 돌아갑니다. 첫 입장에서는 `reconnect`를 생략하거나 `false`를 보냅니다.

## 매치메이킹과 Actor DB

클라이언트가 `FindMatch`를 보내면 `MatchQueued`로 티켓을 받습니다. 첫 패킷이 `FindMatch`이면 Session이 플레이어 ID를 생성합니다. `MatchQueued`를 받은 뒤 `SessionInfoRequest`로 ID를 확인합니다. 대기 중인 티켓은 `CancelMatch`로 취소합니다.

Matchmaker node의 Pool은 Instance Spot입니다. 두 플레이어를 선택하면 play node의 `RoomProvisioner`에 `PrepareRoom`을 보냅니다. `RoomProvisioner`가 Table을 만들고 `RoomPrepared`를 응답합니다. 두 Actor의 입장이 완료되면 Table은 `MatchFound`와 `TableView`로 좌석을 알립니다.

Actor는 첫 입장이나 매치 신청 시 `LoadPlayer`로 API node에서 잔액을 한 번 읽습니다. 판이 끝나면 Table이 모든 좌석의 잔액을 담아 `CommitRound`를 한 번 보냅니다. API node는 Redis에 판의 기록과 잔액을 함께 저장합니다. Table은 `RoundCommitted`를 받은 뒤 잔액을 반영하고 `HandResult`를 보냅니다. 새 게임에는 고유한 테이블 ID를 사용합니다.

## 시나리오

`./run-e2e.sh`는 격리 환경에서 `table`, `reconnect`, `match`, `match-cancel`, `persist`, `update`를 실행합니다. `persist`는 같은 Redis 플레이어 레코드를 쓰는 별도 API node, play node, Session을 시작합니다. 위치 상태는 새로 생성합니다. 이전 좌석이 사라졌는지 확인하고, 새 Actor가 정산 잔액을 읽는지 확인합니다.

[C# 포맷 검사](README.md#c-formatting)는 저장소에 고정된 CSharpier와 .NET 설정을 사용합니다.

## 알려진 문제

drain 확인에 약 30초가 걸리므로 대기하는 동안 진행 중인 판의 턴 시간이 끝날 수 있습니다. 턴 시계가 있는 게임의 update 시나리오에서는 판을 끝낸 뒤 옮깁니다. 남은 시간이 이전되는지는 시계를 제어하는 도메인 검사에서 확인합니다.

## 포그라운드 서버

Linux 또는 WSL에서는 `./run-server.sh`, Windows에서는 `.\run-server.cmd`로 서버를 시작합니다. `Server ready`가 출력되면 다른 터미널에서 `./run-e2e.sh` 또는 `.\run-e2e.cmd`를 실행합니다. e2e는 `.logs/server.json`의 주소를 읽어 실행 중인 서버에 접속하고 `table`, `reconnect`, `match`, `match-cancel`, `persist`를 검증합니다. `reconnect`의 전체 재시작 검사와 `update`의 노드 교체는 격리 실행에서 수행합니다. 서버 터미널에서 Ctrl+C를 누르면 노드와 Redis를 종료하고 `.logs/server.json`을 삭제합니다.
