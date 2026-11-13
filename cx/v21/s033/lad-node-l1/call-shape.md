# Node L0/L1 call shape — binding 대 Framework ladder

비교 기준은 binding과 L0 route 정합성을 맞춘 커밋 `8eb3525b7e` 및 그 위의 L1 permit 커밋 `8463bd941f`다. 처음 대조에서 L0 send-saturation이 request endpoint를 쓰는 불일치가 발견돼, 측정 전에 L0 client를 raw command routing ID와 command endpoint로 맞췄다(`client/main.js:175-199`, `zlink-framework-server/main.js:63-76`). L1은 이 수정된 L0와 같은 빌드에서 실행한다.

| 단계 | binding call (file:line) | L0 call (file:line) | 동일? | 차이가 속한 복원 기능 |
|---|---|---|---|---|
| Source send | `client/main.js:155-162,175-199` — raw request/command routing ID와 각각의 target endpoint, 새 raw envelope + protobuf body submit | 같은 `client/main.js:155-162,175-199`; L0 env에서도 두 routing ID와 endpoint가 binding과 일치 | 예 | — |
| Target receive | `zlink-raw-server/main.js:64-86,95-117` — 두 ROUTER에서 `recv(DontWait)` batch loop | `zlink-framework-server/main.js:93-127,129-155` — 같은 두 ROUTER, `recv(DontWait)`, batch loop | 예 | — |
| Target reply | `zlink-raw-server/main.js:77-86` — 새 protobuf body, 새 `Received.reply()` operation, 두 part submit | `zlink-framework-server/main.js:107-120` — 같은 wire/reply, 고정 `EchoHandler.handleL0()` 직접 호출 | handler 호출만 다름 | L0에서 허용한 고정 handler 직접 호출 |
| Source completion / reply receive | `client/main.js:225-260,272-275` — completion poll, reply body decode, 각 part close | 같은 `client/main.js:225-260,272-275` | 예 | — |

L0R는 `@zlink-systems/framework`와 동일한 queue implementation 모듈을 로드하되 queue를 생성하지 않는다. L1 receive는 `main.js:157-214`에서 매 receive 전에 `ApplicationJobQueue.acquire(undefined, 'remote')`를 호출하고, handler 호출 직전에 `releaseBeforeHandler()`를 호출한다. L0 대비 이 permit 동작만 추가된다. L0와 L1 모두 binding과 같은 setImmediate pump 및 일반 JS metrics counter를 쓴다.

| 경로 | 대기 / 할당 / 원자 연산 / 복사 |
|---|---|
| Source 요청 | 양쪽 같은 client path. 요청 operation과 completion promise, 두 part를 submit하며 JS atomic 연산 없음. completion은 poller와 `sleepImmediate`로 확인. |
| Target receive | binding/L0는 reusable `Received`, `DontWait`, 최대 256개 batch, `setImmediate`; permit 없음. L1은 각 receive 앞에 실제 Framework queue permit을 await하고 no-data면 반환. JS atomic 연산 없음. |
| Target reply | 모두 body를 protobuf Buffer로 새로 encode하고 새 reply operation에 envelope/body 두 part를 submit. request parts를 재사용하지 않음. |
| Source completion | 모두 completion poller와 promise를 사용하고 reply parts를 decode 후 close. JS atomic 연산 없음. |
