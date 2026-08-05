# Event Sample Scenarios

[샘플 목록](../README.ko.md)

이 디렉토리는 event 전파를 중심으로 framework 기능을 보여 주는 공통 샘플 시나리오를
정의한다. 두 샘플은 event를 다루지만 목적이 다르다.

이 디렉토리의 문서도 상위 sample spec과 같은 작성 기준을 따른다. `.NET` Bingo와
TicTacToe 샘플처럼 서버 역할, 연결 방식, 메시지 계약, 흐름, client 시나리오, 구현 완료
기준을 한 문서 안에서 확인할 수 있어야 한다. 차이는 event의 기준 경로뿐이다.
durable event가 필요한 업무 흐름은 도메인 event store, state owner, projection으로 필요한
의미를 먼저 구성한다. event 양과 consumer 수가 커지면 Redis Stream 또는 Kafka를 확장 경로로
둔다. 유실되어도 snapshot으로 보정할 수 있는 realtime 흐름은 fanout이나 owner routing 중
도메인의 소유권 경계에 맞는 ZLink 메시징 경로를 기준으로 둔다. event sourcing이 도메인 모델을
더 선명하게 만드는 경우에는 event stream을 상태의 기준으로 두고 projection은 다시 만들 수 있는
조회 모델로 둔다.

| 샘플 | 목적 | event 기준 경로 | ZLink 역할 |
|------|------|----------------|------------|
| [ShoppingMall](shoppingmall.ko.md) | `CommerceApi`(HTTP edge)와 `OrderWorkflow`(주문 owner)를 분리해 견고한 event-sourced 주문 workflow를 구성한다. | ZLink owner routing + OrderEventStore | event sourcing, workflow owner spot, projection 조회 |
| [GameQuest](gamequest.ko.md) | gameplay event를 player별 owner spot에 모아 event sourced quest aggregate를 갱신한다. | ZLink owner routing + QuestEventStore | owner spot 직렬화, event sourcing, WebSocket notify |

ShoppingMall은 Kafka를 그대로 복제하지 않는다. 작은 규모에서 시작하는 커머스 서비스도
주문, 재고, 결제 workflow는 실패와 중복 요청을 견고하게 처리해야 한다. 이 샘플은 HTTP를 종단하는
`CommerceApi`와 주문 owner인 `OrderWorkflow` 서버를 분리하고, `OrderWorkflowSpot`·`OrderEventStore`·
projection으로 주문 상태 전이를 event sourced workflow로 구성한다. Redis Stream 또는 Kafka는 다수
consumer, 큰 backlog, 외부 downstream replay가 필요할 때 붙이는 확장 선택지다.

GameQuest는 owner-routed gameplay event와 event sourcing을 함께 보여 주는 샘플이다. `Session
Server`가 combat, inventory, mission, world action을 처리하고 gameplay event를 `PlayerId` 기준
owner spot으로 보낸다. `PlayerQuestSpot`은 quest event stream을 replay해 aggregate를 복원한 뒤 새
quest domain event를 append한다. client 조회와 notify는 projection을 사용하고, 누락 가능성은
snapshot 재동기화 결과를 다시 event stream에 append해 보정한다.
