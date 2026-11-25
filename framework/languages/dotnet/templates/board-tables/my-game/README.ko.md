# MyGame 보드 테이블

## 메시지 계약과 연결 종료

1. 공유 DTO에 null 허용과 JSON 표현을 선언합니다.
   - null을 허용하는 필드는 `string?`과 같은 nullable 참조 타입이나 `long?`과 같은 nullable 값 타입으로 선언합니다. 필드 생략을 허용하려면 생성자 매개변수에 기본값을 지정합니다.
   - 64비트 정수는 JSON에서 10진 문자열로 표현합니다. 브라우저 클라이언트도 `"handNumber": "42"`처럼 문자열을 보냅니다. [메시지 모델](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/#12-message-value-rules)

시나리오 하나만 실행하려면 `./run-e2e.sh --scenario reconnect`를 사용합니다. 마지막 request의 응답을 받은 뒤 연결을 닫습니다. send 직후 연결을 닫으면 서버가 메시지를 처리하지 못할 수 있습니다.

[영어 README](README.md)에서 빌드, e2e 실행, 배포 절차를 확인합니다.

이전에 배정된 좌석으로 재접속할 때는 `JoinTableRequest`에 `reconnect: true`를 보냅니다. Actor가 속한 Table에서 요청을 처리합니다. 소속 Table이 없으면 Entry가 `ActionRejected`(`not-seated`)로 응답합니다. 이전 테이블 ID를 버리고 테이블 선택으로 돌아갑니다. 첫 입장에서는 `reconnect`를 생략하거나 `false`를 보냅니다.

## 알려진 문제

drain 확인에 약 30초가 걸리므로 대기하는 동안 진행 중인 판의 턴 시간이 끝날 수 있습니다. 턴 시계가 있는 게임의 update 시나리오에서는 판을 끝낸 뒤 옮깁니다. 남은 시간이 이전되는지는 시계를 제어하는 도메인 검사에서 확인합니다.

## 포그라운드 서버

Linux 또는 WSL에서 `./run-server.sh`로 서버를 시작합니다. 다른 터미널에서 `./run-e2e.sh --attach <endpoint>`를 실행하고 서버가 출력한 WebSocket 주소를 지정하면 `table`과 `reconnect`를 검증합니다. 재시작 검사와 `update`는 격리 실행에서만 수행합니다. 서버 터미널에서 Ctrl+C를 눌러 노드와 Redis를 종료합니다.
