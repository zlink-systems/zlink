# MyGame 보드 테이블

## 메시지 계약과 연결 종료

1. 공유 DTO에 null 허용과 JSON 표현을 선언합니다.
   - null을 허용하는 DTO 필드는 `std::optional<T>`로 선언합니다. 필드 생략을 허용하려면 `dto_traits`에서 별도로 `zlink::json::defaulted`를 지정합니다.
   - 64비트 정수는 JSON에서 10진 문자열로 표현합니다. 브라우저 클라이언트도 `"handNumber": "42"`처럼 문자열을 보냅니다. [메시지 모델](https://docs.zlink.systems/cpp/guide/server/20-channel-messaging/#12-message-value-rules)

시나리오 하나만 실행하려면 `./run-e2e.sh --scenario reconnect`를 사용합니다. 마지막 request의 응답을 받은 뒤 연결을 닫습니다. send 직후 연결을 닫으면 서버가 메시지를 처리하지 못할 수 있습니다.

[영어 README](README.md)에서 빌드, e2e 실행, 배포 절차를 확인합니다.

이전에 배정된 좌석으로 재접속할 때는 `JoinTableRequest`에 `reconnect: true`를 보냅니다. Actor가 속한 Table에서 요청을 처리합니다. 소속 Table이 없으면 Entry가 `ActionRejected`(`not-seated`)로 응답합니다. 이전 테이블 ID를 버리고 테이블 선택으로 돌아갑니다. 첫 입장에서는 `reconnect`를 생략하거나 `false`를 보냅니다.

## 알려진 문제

- 이 템플릿에는 아직 `run-server`가 없습니다. `run-e2e`(Docker 격리 실행)를 사용합니다. 다른 템플릿의 매치메이킹·API node·`run-server`/attach 흐름은 [#5647](https://github.com/zlink-systems/zlink/issues/5647)에서 다룹니다.

drain 확인에 약 30초가 걸리므로 대기하는 동안 진행 중인 판의 턴 시간이 끝날 수 있습니다. 턴 시계가 있는 게임의 update 시나리오에서는 판을 끝낸 뒤 옮깁니다. 남은 시간이 이전되는지는 시계를 제어하는 도메인 검사에서 확인합니다.
