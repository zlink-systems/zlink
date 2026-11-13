# Node L0 / L0R / L1 호출 형태 대조

기준은 HEAD `c512887606aeb686eccc53162d9f02c3c4db63ed`다. binding 경로는 `zlink-raw-server`, L0/L0R/L1은 `zlink-framework-server`의 ladder 경로다. L0R은 L0의 top-level Framework 및 queue 모듈 require만 추가하고, L1은 L0R에 permit 획득·반납만 추가한다.

| 단계 | binding 호출 (file:line) | L0 / L0R / L1 호출 (file:line) | 같은가? | 차이는 복원된 기능인가? |
|---|---|---|---|---|
| Source send | `client/main.js:170-200,243-269` — ROUTER, request 또는 command routing ID/endpoint, envelope와 protobuf body를 새로 구성해 submit | `client/main.js:170-200,243-269` — L0 client에서 raw request/command peer ID와 endpoint를 사용하고 같은 wire를 submit | 예 | — |
| Target receive | `zlink-raw-server/main.js:61-119,132-142` — 재사용 Received, 두 ROUTER에서 DontWait batch pump, setImmediate 재예약 | `zlink-framework-server/main.js:63-155,228-242` — 같은 두 ROUTER, 재사용 Received, DontWait, batch, setImmediate pump | 예 (L0/L0R) | L1만 receive 앞 queue permit 대기 |
| Target reply | `zlink-raw-server/main.js:77-86` — 새 protobuf body encode, reply/send operation 생성, 두 part submit | `zlink-framework-server/main.js:107-120` — body decode 뒤 고정 `EchoHandler.handleL0()` 호출 후 새 body encode 및 `Received.reply()`로 두 part submit | 고정 handler 호출만 다름 | L0 고정 handler 직접 호출. L0R 동일. L1은 permit을 handler 첫 지시 직전에 반환하는 것만 추가 |
| Source completion / reply receive | `client/main.js:202-260,272-275` — completion poller 및 promise 처리, reply body decode 후 모든 part close | `client/main.js:202-260,272-275` — 같은 completion poller, decode, part close | 예 | — |

| 경로 | 대기 / 할당 / 원자 연산 / 복사 |
|---|---|
| Source 요청 | 양쪽 동일한 client 경로. 요청 operation, completion promise, envelope/body part를 생성한다. JS 원자 연산 없음. |
| Target receive | binding/L0/L0R는 재사용 Received, DontWait, 최대 256개 batch, setImmediate pump이며 queue wait 없음. L1은 `ApplicationJobQueue.acquire(undefined, 'remote')`를 매 receive 앞에 await하고, no-data면 unqueued permit을 반환한다. JS 원자 연산 없음. |
| Target reply | 모두 새 protobuf body Buffer와 reply operation을 만들고 두 part를 submit한다. 요청 parts 재사용 없음. |
| Source completion | 모두 completion poll 및 promise, reply parts decode 후 close. JS 원자 연산 없음. |

L0R/L1의 top-level load는 `zlink-framework-server/main.js:18-32`다. permit 획득·반납은 `:157-214`, L1 pump 스케줄 분기는 `:216-242`다. 이 표의 경로와 호출은 측정 전에 HEAD에서 확인했다.
