# Node.js bindings sample 범위

이 디렉터리는 Core 11의 raw socket public API 사용법만 보여 준다.
`PAIR`, `PUB/SUB`, `DEALER/ROUTER`, `STREAM`과 socket monitor 예제를 빌드하고 실행한다.

Actor, Spot, session binding, timer와 Logical Multicast는 Framework가 구현하는 service 기능이다.
이 시나리오는 삭제하지 않았으며 다음 Framework sample이 정본이다.

| 기존 bindings sample의 목적 | Framework sample 정본 |
|---|---|
| Actor가 room Spot에 join하고 요청을 순서대로 처리 | `framework/languages/node/samples/TicTacToe.Ts` |
| Actor queue와 room lifecycle | `framework/languages/node/samples/Bingo.Ts` |
| stream session과 Actor binding·relay | `framework/languages/node/samples/SupportChat.Ts` |
| Spot request/reply와 timer | `framework/languages/node/samples/TicTacToe.Ts` |
| Spot publish와 여러 subscriber 전달 | `framework/languages/node/samples/TicTacToe.Ts` |

bindings sample에 Framework service 기능을 다시 구현하거나 제거된 Core service API를
복원하지 않는다. Framework sample은 Framework public API와 실제 runtime을 함께 검증한다.
