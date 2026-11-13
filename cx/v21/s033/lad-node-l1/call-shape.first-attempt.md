# Node L0/L1 호출 모양 — binding 대조

비교 기준은 branch `exp/4350-node`의 L0 commit `dba747fe85`와 L1 후보다. L1은 L0의 직접 ROUTER 서버를 유지하고 application job queue permit의 수신 전 획득과 handler 직전 반환만 추가한다.

| 단계 | binding 호출 (`zlink-raw-*`) | L0 호출 | 동일? | L1의 유일한 차이 |
|---|---|---|---|---|
| source send (request) | `client/main.js:243-249` ROUTER `.request().message(envelope).message(body).submit()` | 동일 client 경로 `client/main.js:243-249`; ladder client는 `:172-186`에서 raw binding transport 선택 | 예 | 없음 |
| target receive | `zlink-raw-server/main.js:47-53,64-92,132-145` — `recv(DontWait)`를 batch pump로 호출하고 `setImmediate`로 재예약 | `zlink-framework-server/main.js:76-110,139-145` — 두 ROUTER를 같은 pump로 수신 | 예 (wait primitive·pump·batch 일치) | 각 receive 전에 ApplicationJobQueue permit 획득 |
| target reply | `zlink-raw-server/main.js:77-86` — body decode/encode 후 `Received.reply().message(envelope).message(body).submit()` | `zlink-framework-server/main.js:90-100` — 동일 두 part 회신, 고정 `EchoHandler.handleL0` 직접 호출 | 호출·part 수 동일; 고정 handler 호출은 L0 허용 차이 | handler의 첫 instruction 직전에 `permit.releaseBeforeHandler()` |
| source completion / reply receive | `client/main.js:225-241,250-260,272-275` — completion poll 후 reply body decode 및 part close | 동일 client 경로 | 예 | 없음 |

메시지당 두 경로 모두 send에는 envelope와 protobuf body 두 part를 만들고, request receive/reply에는 `Received`를 재사용하며, source reply part를 닫는다. L0 수신은 setImmediate pump라 blocking wait를 추가하지 않는다. L1 permit은 Framework의 `ApplicationJobQueue` / `ApplicationJobQueuePermit` (`packages/framework/src/runtime/host/application-job-queue.ts`)를 사용하며, permit 획득·반환만 추가 비용이다. L1 코드는 `main.js`의 env switch로 같은 빌드에서 L0/L1을 선택한다.
