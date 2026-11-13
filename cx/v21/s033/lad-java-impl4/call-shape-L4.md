# Java ladder L4 호출 형태

L4는 L3의 blocking receive, service-wire multipart와 reply 틀을 유지하고 typed payload JSON 직렬화·역직렬화만 복원한다. L1 permit과 L2 envelope도 이전 단계 차이로 남는다. L5 owner queue·worker 경유는 아직 복원하지 않았다.

| 경로 | binding bench | L4 bench | 동일 여부와 차이 |
|---|---|---|---|
| source send | `RawStack.java:243-274`은 protobuf payload를 만들고 ROUTER send에 두 part를 제출한다. | `FrameworkStack.java:132-143`이 L4 operation을 고른다. `RawStack.java:309-339,402-413`은 `ZLinkPayloadEncoding.encodeMessage`로 JSON typed payload를 만들고 service header + multipart frame을 제출한다. | 다름: L3 service-wire framing, L4 typed codec. L1/L2 기능은 send 요청 body에 추가하지 않는다. |
| target receive | `ZLinkRawBenchServer.java:94-109,193-198`은 메시지마다 blocking `recv(NONE)` 후 protobuf body를 읽는다. | `ZLinkFrameworkBenchServer.java:136-162,239-290`은 같은 blocking receive 루프에서 service header와 envelope를 해석하고 `ZLinkMessagePayloads.deserialize`로 DTO를 복원한다. | receive primitive와 owner loop는 같다. 차이: L1 permit, L2 envelope, L3 service wire, L4 typed codec. owner queue·worker는 아직 없다. |
| target reply | `ZLinkRawBenchServer.java:115-127`은 새 response envelope와 protobuf body를 만들어 두 part를 제출한다. | `ZLinkFrameworkBenchServer.java:291-316`은 typed JSON body와 response envelope를 새로 인코딩한 뒤 service reply header와 multipart frame을 제출한다. | 다름: L3 framing과 L4 typed codec. 요청 parts 재사용 없이 새 body encode·submit을 유지한다. |
| source completion | `RawStack.java:75-97,383-400`은 raw reply의 protobuf body를 디코드하고 측정 header를 확인한다. | `RawStack.java:142-197,415-428`은 service reply header/frame을 풀고 `ZLinkMessagePayloads.deserialize`로 typed JSON 응답을 디코드한다. | 다름: L3 service-wire reply frame과 L4 typed codec. |

## Framework production 경로

- source typed payload encoding: `ZLinkChannelCalls.java:69-74` → `ZLinkPayloadEncoding.java:49-71`; node send 진입도 `ZLinkChannelCalls.java:157-163`에서 같은 encoder를 쓴다.
- target payload decode: `ZLinkChannelHandlerInvoker.java:421-429`가 content-type serializer를 고르고 `ZLinkMessagePayloads.deserialize`를 호출한다.
- target typed reply encode: `ZLinkChannelHandlerInvoker.java:393-396`에서 `ZLinkMessagePayloads.message`가 handler reply를 serializer로 인코딩한다.
- source reply decode: `ZLinkChannelReplyDecoder.java:47-49`가 같은 serializer를 사용한다.
