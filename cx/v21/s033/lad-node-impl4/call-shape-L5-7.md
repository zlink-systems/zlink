# Node L5~L7 호출 형태 대조

Node에서는 Framework의 owner queue 경로가 raw service mailbox와 Core dispatch pump를 거친 뒤 handler 활성화 및 request completion까지 이어진다. production 내부 진입점을 bench 전용으로 노출하지 않고 L5~L7을 한 단계로 묶었다. `ZLINK_LADDER_L5_7_CLIENT=1` 및 `ZLINK_LADDER_L5_7_SERVER=1`은 기존 public Framework client/server 경로를 선택하고 protobuf 등록을 끈다. L4 기본 JSON payload 경로는 유지한다.

| 경로 | binding raw 호출 | L5~L7 ladder 호출 | 차이와 귀속 |
|---|---|---|---|
| Source send | `client/main.js:313-333`; binding raw socket에 envelope와 protobuf body를 직접 제출 | `client/framework-transport.js:67-73`; public `ZLinkRouteClient.sendToNode(...).submit()` | L2 envelope·L3 service wire·L4 JSON은 누적 복원. L5 source owner turn 및 L7 submit/completion 경로는 그룹 단계 |
| Target receive | `zlink-raw-server/main.js:64-119`; binding receive와 setImmediate pump | `zlink-framework-server/main.js:347-359`; public Nest/Framework host의 request/send handler | L1 permit, L2 header, L3 service frame, L4 JSON decode에 L5 mailbox owner queue·Core owner dispatch 및 L6 handler activation 추가 |
| Target reply | `zlink-raw-server/main.js:77-86`; 새 response envelope와 protobuf body 제출 | `zlink-framework-server/main.js:350-359`; `EchoHandler` 결과를 Framework dispatch가 reply로 반환 | L3 reply frame 및 L4 typed JSON 유지. L5~L6 handler turn/activation을 거치며 L7 reply completion을 Framework가 관리 |
| Source completion / reply receive | `client/main.js:285-307`; raw request promise와 protobuf decode | `client/framework-transport.js:56-63`; public request operation과 Framework reply decode | L3 correlation 및 L4 JSON decode 누적. L7 operation registry·completion dispatcher가 그룹 단계 |

## production 경로

- Source send/request: `channel-outbound-operations.ts:79-100,191-249,370-400,486-525`; public client transport는 `framework-transport.js:56-73`에서 이 경로를 호출한다.
- Owner mailbox: `raw-service-mesh-runtime.ts:1490-1543`에서 owner 및 application permit을 정하고 mailbox에 enqueue한다. `node-raw-mesh-backend.ts:1028-1055,1997-2015`가 owner별 claim을 만든다. `mesh-dispatch-pump.ts:526-560`이 claim records를 turn 단위로 dispatch한다.
- Handler activation: Nest `handler-adapters.ts:258-308`가 per-dispatch handler instance를 만들고 dispose한다. Framework request reply는 `channel-dispatchers.ts:230-245` → `channel-envelope.ts:217-237`를 거친다.
- Operation completion: `request-runtime.ts:17-23,188-190`의 `OperationRegistry`와 `channel-outbound-operations.ts:174-249`의 request/reply receive가 소유한다.
