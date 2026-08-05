# Java RegistrationCodec E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md`

마지막 검증:

- 명령: `nice -n 10 timeout 600s ./run_e2e.sh all`
- 결과: `RC-A1`~`RC-A6`, `RC-B1`~`RC-B5` 통과, `registration-codec e2e result=passed`
- 로그: `framework/languages/java/e2e/RegistrationCodec/logs/20260707-215508-3550080/`
- 명령: `for scenario in RC-B1 RC-B2 RC-B3 RC-B4; do ./run_e2e.sh "${scenario}"; done`
- 결과: passed
- 로그:
  - `framework/languages/java/e2e/RegistrationCodec/logs/20260707-153318-2447031/`
  - `framework/languages/java/e2e/RegistrationCodec/logs/20260707-153347-2448777/`
  - `framework/languages/java/e2e/RegistrationCodec/logs/20260707-153405-2449796/`
  - `framework/languages/java/e2e/RegistrationCodec/logs/20260707-153422-2451535/`

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| RC-A1 | 구현 | Client HTTP driver가 server endpoint를 호출하고 `addHandlersFromPackageOf`와 handler group으로 자동 등록된 request/send handler를 검증한다. |
| RC-A2 | 구현 | Client HTTP driver가 server endpoint를 호출하고 Java `@ZLinkHandlerGroup`, `@ZLinkRequest`, `@ZLinkSend` annotation 등록 handler를 검증한다. |
| RC-A3 | 구현 | Client HTTP driver가 server endpoint를 호출하고 builder의 `addRequestHandler(..., packetName)`와 `addSendHandler(..., packetName)` 명시 등록을 검증한다. |
| RC-A4 | 구현 | Client HTTP driver가 server endpoint를 호출하고 request마다 새 DI 객체를 만들며 singleton id 유지와 dispose count를 evidence로 확인한다. |
| RC-A5 | 구현 | Client HTTP driver가 server endpoint를 호출하고 `useFilter`로 등록한 두 handler filter의 before/after 순서를 evidence로 확인한다. |
| RC-A6 | 구현 | Client scenario가 duplicate packet registration server role을 시작하고 startup failure를 확인한다. |
| RC-B1 | 구현 | JSON DTO request/send 왕복과 handler context의 `application/json` content-type evidence를 확인한다. |
| RC-B2 | 구현 | `StringValue` DTO가 Protobuf codec으로 왕복하고 handler context의 `application/x-protobuf` content-type evidence를 확인한다. |
| RC-B3 | 구현 | typed MessagePack codec factory로 지정한 DTO가 왕복하고 handler context의 `application/x-msgpack` content-type evidence를 확인한다. |
| RC-B4 | 구현 | JSON fallback, Protobuf predicate codec, typed MessagePack codec을 한 host에 같이 등록하고 세 content-type evidence가 함께 남는지 확인한다. |
| RC-B5 | 구현 | Client HTTP driver가 codec requester endpoint를 호출해 json-only peer에 Protobuf request를 보내고 public error와 정상 JSON recovery를 검증한다. |

## Content-type 검증

공통 Config 4의 codec별 content-type 확인은 Java framework의 public handler context인
`ZLinkHandlerContext.contentType()`으로 검증한다. E2E handler는 raw frame이나 private runtime에
접근하지 않고, request/send handler context에 들어온 content-type을 evidence로 남긴다. Client
scenario는 RC-B1~RC-B4에서 JSON, Protobuf, MessagePack content-type이 기대값과 일치하는지 확인한다.

## 공통 scenario parity gap — 2026-07-29

- `RC-B6`: 공통 scenario는 추가됐지만 Java actual fixture와 runner selector가 없다.
