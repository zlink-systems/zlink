# Protocol — ZMP v1.0 스펙-구현 gap 감사

> 감사 도구: codex (정적 코드 대조) · 2026-08-24
> 범위: `core/doc/spec/core/protocol/01-zmp.ko.md`, `core/include/`, `core/src/` 및 관련 정적 test 표본. 실행 테스트는 수행하지 않았다.

판정: **구현/문서 gap 8건 (A 3건, B 2건, C 3건)**. header와 envelope의 정상 경로는 encoder/decoder의 실제 byte 배치까지 대조했다. 코드와 대상 스펙은 수정하지 않았고, 이 보고서만 작성했다.

## 대조 완료 계약군

- ZMP header의 정상 송신 배치 — MAGIC `0x5A`, VERSION `0x01`, FLAGS, RESERVED `0x00`, offset 4의 32-bit big-endian 길이 — 는 encoder와 일치한다. 단, 32-bit 상한 검증은 아래 B 항목이다.
- 정상 header의 decode, `CONTROL|MORE` 거부, request-reply envelope의 4 part 순서와 `uint64` big-endian `request_seq` decode는 일치한다.
- protocol id/version/type 값, `request_seq == 0` 거부, envelope part 길이(앞 3개 1 byte, seq 8 byte), reply의 Completion lane 전송은 일치한다.
- paired DEALER/ROUTER의 두 lane 생성, pair metadata의 64-bit big-endian 인코딩, 두 lane 검증 전 Application write hold, peer identity 대조, Completion lane 독립 처리는 코드에 있다.
- WebSocket transport는 Beast stream을 binary mode로 설정하고 한 write buffer를 binary WebSocket frame으로 전송한다.

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| A. 문서 누락 | `01-zmp.ko.md:74-88`, `270-273` — FLAGS의 이름과 `CONTROL|MORE` 거부만 정의 | `core/src/runtime/protocol/zmp_decoder.cpp:109-144` | decoder의 공개 wire 수용 규칙이 더 넓다. RESERVED가 0이 아니면 거부하고, bit 5-7, `CONTROL|IDENTITY`, `SUBSCRIBE|CANCEL`, 그리고 SUBSCRIBE/CANCEL과 다른 모든 flag의 결합도 `EPROTO`로 거부한다. 상호운용 구현이 어떤 frame을 보내면 안 되는지 문서에 없다. |
| A. 문서 누락 | `01-zmp.ko.md:109-113`, `125-129` — READY property 이름과 pair 값의 byte 크기만 설명 | `core/src/runtime/protocol/zmp_metadata.hpp:56-70,73-100,165-195` | READY metadata의 실제 byte 배치가 문서에 없다. 코드는 각 property를 `[name length: u8][name bytes][value length: u32 BE][value bytes]`로 만들고, metadata 사용 시 `Zlink-Max-Message-Size`도 8-byte BE로 항상 추가한다. 이 layout과 property는 다른 구현의 READY decoder/encoder가 의존할 wire 계약이다. |
| A. 문서 누락 | `01-zmp.ko.md:23`, `84-88` — connection control frame으로 HELLO/READY만 설명 | `core/src/runtime/protocol/zmp_protocol.hpp:20-34`; `core/src/runtime/protocol/zmp_control.hpp:351-355,365-394` | code는 ERROR control type `0x05`와 `[type=0x05][error code: u8][reason length: u8][reason bytes]`를 정의·해석한다. ZMP가 connection control frame을 정의한다고 한 문서에 ERROR type과 body 배치가 없다. |
| B. 구현 gap | `01-zmp.ko.md:54-72`, `270-272` — 모든 ZMP frame의 payload size는 header offset 4-7의 32-bit BE 값 | `core/src/runtime/protocol/zmp_encoder.cpp:19-45`; `core/src/runtime/protocol/zmp_protocol.hpp:12` | encoder는 `size_t size`를 상한 검사 없이 `uint32_t`로 cast한다. 64-bit 환경에서 payload가 `0xffffffff`를 넘으면 header에는 절단된 길이를 쓰지만 body는 원래 길이로 송신한다. 이는 header의 byte 배치와 body 길이가 달라지는 구현 gap이다. |
| B. 구현 gap | `01-zmp.ko.md:171`, `292`, `299` — error reply 첫 payload part는 errno이고 high-level completion은 `errno != 0` | `core/src/api/socket/request_reply_protocol_internal.hpp:177-203`; `core/src/api/socket/socket_request_reply_dispatch.cpp:62-79` | error-reply errno 4 byte를 `0x00000000`으로 보내도 decoder는 그대로 `callback_errno=0`으로 전달한다. 그러면 error reply가 성공 completion이 되어 `errno != 0` 계약을 깨뜨린다. |
| C. 문서-코드 모순 | `01-zmp.ko.md:92-103` — 양쪽 peer가 연결 시 HELLO+READY를 한 outbound buffer로 송신 | `core/src/runtime/engine/asio/asio_zmp_engine.cpp:196-216,410-480`; `core/src/runtime/transports/ws/asio_ws_engine.cpp:827-896` | paired transport의 passive 쪽은 초기 outbound buffer에 HELLO만 넣고 READY 송신을 peer READY 수신 뒤로 지연한다. 따라서 두 쪽 모두가 연결 시 한 buffer로 `HELLO+READY`를 송신한다는 sequence diagram과 다르다. TCP와 WebSocket engine 모두 같은 동작이다. |
| C. 문서-코드 모순 | `01-zmp.ko.md:109-113`, `277-278` — metadata option을 켜면 `Socket-Type`·`Routing-Id` property가 추가됨 | `core/src/runtime/protocol/zmp_metadata.hpp:76-88`; `core/src/runtime/core/options.cpp:125-133` | option의 기본값 false는 일치하지만, 활성화해도 `Routing-Id`는 DEALER/ROUTER에만 추가된다. PAIR/PUB/SUB/XPUB/XSUB/STREAM READY는 `Socket-Type`과 `Zlink-Max-Message-Size`만 갖는다. 문서의 socket-type 비한정 서술과 실제 wire가 다르다. |
| C. 문서-코드 모순 | `01-zmp.ko.md:181-188`, `191-192` — ROUTER→DEALER reply를 `[routing_id] + [envelope 4 parts] + [reply payload]`로 표시하면서 이 sequence의 wire part 배치를 계약으로 지시 | `core/src/api/socket/socket_request_reply_runtime_io.cpp:678-708,725-737,786-807`; `core/src/runtime/engine/asio/asio_zmp_engine.cpp:268-282,510-518` | reply wire에는 envelope와 payload만 write된다. `routing_id`는 ROUTER가 `application_pipe`/Completion pipe를 선택하는 로컬 key이며, engine도 local receive용 routing-id message만 별도로 만든다. diagram처럼 reply wire part 앞에 routing id가 있는 것으로 읽히는 서술은 실제 byte 배치와 모순된다. |

## 요확인

- 없음. 위 판단은 실행 없이 source의 encode/decode·handshake 경로를 직접 대조한 정적 결과다.
