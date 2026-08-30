# ZMP request-reply protocol metadata 전환 계획

> 작성일: 2026-08-30
> 상태: **구현 전 실행 계획**. 이 문서는 공개 계약이 아니며 현재 동작을 변경하지 않는다.
> 독자: 별도 세션에서 Core ZMP request-reply 구조를 수정하고 검증할 개발자
> 완료 질문: application payload 앞의 4개 envelope part를 없애고, ZMP가
> message kind와 request sequence를 직접 전달하도록 어떻게 안전하게 전환하는가?

## 1. 결론

Request-reply의 `request`, `reply`, `error reply` 구분과 `request_seq`는
application multipart가 아니라 ZMP가 소유해야 한다. 송신 API가 받은 application
payload part 수와 수신 API가 돌려주는 part 수는 일반 `send`와 request-reply에서 같아야
한다.

목표 wire 구조는 다음과 같다.

```text
SEND
[ZMP data header][application payload]

REQUEST
[ZMP request header + request_seq][application payload]

REPLY
[ZMP reply header + request_seq][application payload]
```

현재 4개 envelope part를 하나의 application part로 합치는 작업이 아니다. 해당 정보를
application multipart에서 완전히 제거하고 ZMP frame header의 protocol metadata로
옮긴다. ZMP decoder는 이를 Core 내부 message metadata로 복원하며, public `Message`에는
노출하지 않는다.

## 2. 이번 계획에서 고정한 결정

| ID | 결정 |
|---|---|
| D1 | 이 작업은 아직 공개되지 않은 ZMP 계약을 직접 수정한다. 구버전 peer 협상, capability fallback, 기존 4-part decode 호환 경로를 만들지 않는다 |
| D2 | 제품·패키지 release version을 이 작업 때문에 올리지 않는다 |
| D3 | `ZMP v2`는 이번 구조 개편의 작업명이다. 미공개 ZMP 계약을 제자리에서 수정하며 `zmp_version == 0x01`과 제품 version을 유지한다. 별도 migration을 만들지 않는다 |
| D4 | ordinary `send`, `request`, `reply`는 application payload part 수와 byte를 그대로 전송한다 |
| D5 | request-reply marker와 sequence는 Core 내부 전용이다. `zlink_msg_set_metadata()` 같은 public message metadata API를 복원하지 않는다 |
| D6 | Framework의 `ZLinkMessageMetadata`와 ZMP request metadata를 합치지 않는다. Framework metadata는 application protocol, request metadata는 transport protocol이다 |
| D7 | Completion lane, pending map, timeout, callback과 first-completion-wins 의미는 유지한다. 이번 변경은 wire 표현과 판별 책임만 옮긴다 |
| D8 | C perf의 application payload 정책은 바꾸지 않는다. 기본 측정은 계속 `[1024-byte payload][empty part]` 두 part다 |
| D9 | Core 로컬 build는 실행하지 않는다. 구현 후 build·test는 사용자가 승인한 commit/push와 GitHub Actions artifact로 검증한다 |

`D3`의 의미는 현재 ZMP source와 계약을 제자리에서 수정한다는 것이다. 호환 분기와
legacy 이름을 남기지 않으며, source의 `zmp_version` byte와 제품·패키지 version도
바꾸지 않는다. 작업 중 version 숫자를 바꿔야 한다는 별도 요구가 생기면 이 계획의
범위를 변경한 뒤 진행한다.

## 3. 현재 구현에서 확인된 문제

### 3.1 현재 request-reply wire 배치

Application payload가 한 part이면 현재 request는 다섯 part다.

```text
[protocol id: 1 B]
[version: 1 B]
[message type: 1 B]
[request_seq: 8 B]
[application payload]
```

C perf는 모든 pattern을 multipart로 측정하기 위해 빈 part 하나를 추가한다.

```text
SENDSEND perf
[1024-byte payload][empty]                         = 2 frames

REQREP perf
[id][version][type][seq][1024-byte payload][empty] = 6 frames
```

왕복 기준 SENDSEND는 4 frame, REQREP는 12 frame이다. 11바이트 고정 metadata가
8개의 추가 wire frame, message 객체 생성과 multipart 상태 전이를 만든다.

### 3.2 현재 판별 방식

수신측 `parse_envelope()`는 전체 multipart의 앞 네 part가 다음 조건을 만족하는지
검사한다.

1. 첫 part가 크기 1, 값 `0x01`이다.
2. 둘째 part가 크기 1, 값 `0x01`이다.
3. 셋째 part가 크기 1이며 request/reply/error 중 하나다.
4. 넷째 part가 크기 8이며 decode한 sequence가 0이 아니다.

조건이 맞으면 request-reply, 아니면 raw message로 처리한다. envelope part는 ZMP
`CONTROL` frame이 아니라 일반 data frame이다. 따라서 일반 application multipart가
같은 배치를 사용하면 request로 오인할 수 있다.

주요 source:

- `core/src/api/socket/request_reply_protocol_internal.hpp`
- `core/src/api/socket/socket_request_reply_submit_api.cpp`
- `core/src/api/socket/socket_request_reply_runtime_io.cpp`
- `core/src/api/socket/socket_request_reply_dispatch.cpp`
- `core/src/api/socket/request_reply_frame_buffer_internal.hpp`

### 3.3 현재 ZMP header

ZMP data frame은 8바이트 header를 사용한다.

```text
 Byte:   0         1         2         3         4    5    6    7
      +---------+---------+---------+---------+---------------------+
      |  MAGIC  | VERSION |  FLAGS  |RESERVED |   PAYLOAD SIZE      |
      +---------+---------+---------+---------+---------------------+
```

- `FLAGS` bit 0~4는 MORE, CONTROL, IDENTITY, SUBSCRIBE, CANCEL이다.
- bit 5~7은 현재 거부한다.
- byte 3은 반드시 0이어야 한다.
- payload size는 application frame body 크기다.

주요 source:

- `core/src/runtime/protocol/zmp_protocol.hpp`
- `core/src/runtime/protocol/zmp_encoder.cpp`
- `core/src/runtime/protocol/zmp_decoder.cpp`
- `core/src/runtime/transports/{tcp,ipc,ws,tls}/`

### 3.4 이전 message metadata 구현

`fa77a2a767`은 `zlink_msg_set_request()`, `zlink_msg_set_reply()`,
`zlink_msg_set_metadata()`와 내부 envelope encode/decode를 추가했다. 다음 커밋
`cca289f4f4`가 이 API와 구현을 제거하고 현재의 4-part ZMP envelope를 도입했다.

이번 작업은 이전 public API를 복원하지 않는다. 이전 구현은 metadata ownership,
`msg_t` copy/move/reset 처리와 실패 사례를 확인하는 참고 자료로만 사용한다.

## 4. 목표 계약

### 4.1 Application 계약

- 일반 send에 payload N개를 넘기면 peer는 payload N개를 받는다.
- request에 payload N개를 넘기면 request handler는 payload N개를 받는다.
- reply에 payload N개를 넘기면 completion callback은 payload N개를 받는다.
- payload의 byte 내용은 ZMP metadata 때문에 바뀌지 않는다.
- request API가 받은 첫 payload가 우연히 protocol signature와 같은 byte로 시작해도
  payload로 보존한다.
- raw send API로 request marker를 위조할 public 표면은 제공하지 않는다.

### 4.2 ZMP 내부 계약

ZMP header는 일반 data와 request-reply data를 구분한다. 권장 배치는 현재 byte 3을
`MESSAGE KIND`로 사용하고, request-reply kind에만 8바이트 sequence extension을
붙이는 방식이다.

```text
DATA
 Byte:   0         1         2         3         4    5    6    7
      +---------+---------+---------+---------+---------------------+
      |  MAGIC  | VERSION |  FLAGS  |  DATA   |   PAYLOAD SIZE      |
      +---------+---------+---------+---------+---------------------+

REQUEST / REPLY / ERROR REPLY
 Byte:   0         1         2         3         4    5    6    7
      +---------+---------+---------+---------+---------------------+
      |  MAGIC  | VERSION |  FLAGS  |  KIND   |   PAYLOAD SIZE      |
      +---------+---------+---------+---------+---------------------+
      |                 REQUEST SEQUENCE (64-bit BE)                |
      +-------------------------------------------------------------+
      |                    APPLICATION PAYLOAD ...                  |
      +-------------------------------------------------------------+
```

초기 kind 값 제안:

| 값 | 의미 | sequence extension |
|---:|---|---|
| `0x00` | data | 없음 |
| `0x01` | request | 필수, 0 금지 |
| `0x02` | reply | 필수, 0 금지 |
| `0x03` | error reply | 필수, 0 금지 |

세부 규칙:

- payload size는 extension을 제외한 application payload byte 수다.
- max-message-size와 HWM admission charge도 extension을 제외한 application payload
  byte와 기존 `sizeof(msg_t)`만 사용한다.
- request metadata는 multipart의 첫 data frame에만 기록한다.
- 이어지는 frame은 `kind=data`이며 기존 `MORE` 규칙을 따른다.
- request-reply kind는 `CONTROL`, `IDENTITY`, `SUBSCRIBE`, `CANCEL`과 함께 사용할 수 없다.
- multipart 중간 frame에 request-reply kind가 나타나면 `EPROTO`다.
- `IDENTITY`와 `CONTROL` frame은 application multipart 시작으로 계산하지 않는다.
- application multipart가 시작된 뒤 마지막 data frame을 받기 전에 `IDENTITY`나
  `CONTROL` frame이 나타나면 `EPROTO`다.
- 마지막 application data frame을 처리하면 decoder의 multipart 상태를 반드시
  초기화한다.
- `request_seq == 0`, 알 수 없는 kind, 잘린 extension은 `EPROTO`다.
- 일반 data header 크기는 계속 8바이트다. request-reply 첫 frame만 16바이트다.
- TCP, IPC, WS, WSS가 같은 byte 배치를 사용한다. WebSocket에서는 확장 header와
  payload가 같은 RFC 6455 binary message 안에 기록된다.
- error reply의 errno payload 표현을 header로 옮기는 작업은 이번 범위에 넣지 않는다.
  기존 error completion 의미를 그대로 유지한 뒤 별도 판단한다.

이 배치는 구현 전 Core 소유자 관점에서 한 번 검토한다. 더 일반적인 TLV metadata
extension은 향후 확장에는 유리하지만, 모든 frame에 길이·type parser를 추가하고
request-reply에 필요 없는 동적 구조를 만든다. 현재 요구에는 고정 kind와 sequence가 더
작고 검증하기 쉽다.

## 5. 내부 message metadata 저장 원칙

ZMP decoder가 읽은 kind와 sequence는 payload와 함께 socket 계층까지 이동해야 한다.
그러나 이를 위해 `msg_t`를 키우거나 small-message 저장 한계를 낮추면 안 된다.

현재 `msg_t`는 64바이트이며 `max_vsm_size`는 29바이트다. Core HWM도
`sizeof(msg_t)`를 message별 고정 비용으로 계산한다. 따라서 다음 조건을 구현 gate로 둔다.

- `sizeof(msg_t) == 64`를 유지한다.
- `msg_t::max_vsm_size == 29`를 유지한다.
- 일반 data message의 init/copy/move/close hot path에 heap allocation을 추가하지 않는다.
- 일반 data message의 encoder 분기에 request 전용 mutex나 map lookup을 추가하지 않는다.

우선 검토할 구현은 현재 16바이트 `group_t` 영역을 용도별 내부 auxiliary storage로
정리하는 것이다. group metadata와 request metadata는 같은 message에서 동시에 필요하지
않지만, 현재 `msg_t::close()`와 `copy()`는 `group.type == group_type_long`이면 해당
영역을 pointer로 해석한다. 따라서 discriminator 없는 단순 union은 금지한다.

16바이트 전체 안에 소유 형태를 나타내는 `auxiliary_kind`를 두고, 나머지 영역을 group과
request-reply가 나누어 쓰는 형태를 우선 검토한다.

```cpp
struct message_auxiliary_t
{
    auxiliary_kind_t kind;
    union
    {
        group_storage_t group;
        request_reply_metadata_t request_reply;
    } value;
};
```

정확한 field 순서와 padding은 구현 단계에서 `sizeof(message_auxiliary_t) <= 16`을
static assertion으로 확인한다. request/reply kind 값이 기존 `group_type_long`과 같은
숫자여도 `auxiliary_kind`가 다르므로 group pointer로 해석되지 않아야 한다.

채택 전에 다음을 증명해야 한다.

1. request-reply가 허용되는 DEALER/ROUTER 경로는 group metadata를 사용하지 않는다.
2. RADIO/DISH group message는 request-reply metadata를 설정할 수 없다.
3. `close()`와 `copy()`는 `auxiliary_kind == group`일 때만 long-group pointer의
   refcount를 변경하거나 해제한다.
4. init, copy, move, close와 zero-copy decoder가 auxiliary kind를 정확히 보존하거나
   초기화한다.
5. public `group()`은 request metadata message를 group으로 해석하지 않으며 기존 group
   동작과 ABI 크기가 변하지 않는다.
6. long group과 request metadata 각각의 init/copy/move/close test가 존재한다.

이 조건을 만족하지 않으면 decoder/session sidecar를 대안으로 비교한다. sidecar가 frame마다
allocation 또는 별도 queue element를 만들면 채택하지 않는다. request metadata를 위해
모든 message의 VSM 용량을 줄이는 구현도 채택하지 않는다.

## 6. 구현 단계

### Phase 0. 작업 보호와 계약 승인

1. `git branch --show-current`와 `git status --short`를 확인한다.
2. 현재 worktree에는 bindings 성능 작업이 많이 존재하므로 범위를 섞지 않는다.
3. 새 branch·worktree가 필요하면 사용자 승인을 먼저 받는다.
4. 다음 보호 문서의 수정은 경로와 범위를 사용자에게 명시해 승인받은 뒤 수행한다.
   - `core/doc/spec/core/protocol/01-zmp.ko.md`
   - `bindings/doc/spec/README.ko.md`
   - 필요한 경우 대응 영문 문서
5. 구현 전 이 문서의 D1~D9와 §4 wire 배치를 다시 확인한다.

완료 조건: compatibility fallback과 version release 작업이 범위에 들어오지 않았으며,
보호 문서 승인 여부가 기록돼 있다.

### Phase 1. ZMP metadata value와 `msg_t` 내부 표현

1. `zmp_protocol.hpp`에 data/request/reply/error kind와 request sequence 크기를 정의한다.
2. `msg_t`에 public API가 아닌 내부 setter/getter/reset 경로를 추가한다.
3. init/copy/move/close/view/external-storage 경로의 metadata 수명을 맞춘다.
4. group auxiliary storage를 사용한다면 별도 discriminator를 두고 동시 사용을 assertion과
   test로 금지한다.
5. long group과 request metadata 각각의 init/copy/move/close test를 추가한다.
6. `sizeof(message_auxiliary_t) <= 16`, `sizeof(msg_t) == 64`와 `max_vsm_size == 29`
   고정 test를 추가한다.

완료 조건:

- request metadata round-trip 단위 test가 통과한다.
- 일반 message copy/move 결과가 기존과 같다.
- request metadata close가 long-group pointer 해제 경로에 들어가지 않는다.
- `msg_t` 크기와 VSM 경계가 변하지 않는다.

### Phase 2. ZMP encoder·decoder

1. ZMP data header 생성은 공통 `build_zmp_data_header()` 계열 helper 한 곳이 소유하게
   한다. generic encoder, Asio ZMP engine과 WS engine이 이 helper를 사용한다.
2. encoder가 일반 data에는 기존 8바이트 header를 생성한다.
3. 첫 request-reply data frame에는 kind와 sequence extension을 생성한다.
4. decoder는 base header를 먼저 읽고 kind에 따라 sequence 8바이트를 추가로 읽고
   검증한 뒤 frame admission을 요청한다.
5. decoder가 payload에는 extension을 포함하지 않고 내부 metadata만 복원한다.
6. max-message-size 검사와 HWM admission은 application payload 크기만 사용한다.
7. multipart 시작/계속 상태를 추적해 중간 frame metadata와 multipart 진행 중의
   identity/control frame을 거부한다.
8. control/identity/subscribe/cancel 조합과 잘린 extension을 거부한다.
9. extension 검증 실패 시 frame reservation을 남기지 않는다.
10. backpressure 뒤 `retry_frame_admission()`은 kind, sequence, payload 크기와 read
    cursor를 보존하고, frame별 admission callback은 정확히 한 번만 성공한다.
11. TCP/IPC의 gather write와 WS/WSS header buffer가 최대 16바이트 header를 수용하도록
    수정한다. 일반 data fast path의 gather 구조는 유지한다.

주요 수정 후보:

- `core/src/runtime/protocol/zmp_protocol.hpp`
- `core/src/runtime/protocol/zmp_encoder.{hpp,cpp}`
- `core/src/runtime/protocol/zmp_decoder.{hpp,cpp}`
- `core/src/runtime/core/msg.{hpp,cpp}`
- `core/src/runtime/engine/asio/asio_zmp_engine.{hpp,cpp}`
- `core/src/runtime/transports/tcp/`
- `core/src/runtime/transports/ipc/`
- `core/src/runtime/transports/ws/`
- `core/src/runtime/transports/tls/`

완료 조건: 일반 data의 byte golden test는 header byte 3이 0인 기존 결과를 유지하고,
request-reply golden test는 payload 앞에 application envelope를 만들지 않는다. source
검색에서 ZMP data header를 독립적으로 조립하는 중복 구현이 남지 않는다. control frame
header helper는 data metadata를 만들지 않는 별도 경로로 유지할 수 있다.

### Phase 3. Request-reply 송신 경로 전환

1. `init_envelope_control_parts()`와 `control_part_count=4` 의존을 제거한다.
2. request submit 시 첫 application part에 내부 `request` metadata를 설정한다.
3. reply submit 시 첫 application part에 내부 `reply` metadata를 설정한다.
4. error reply도 기존 errno payload 의미를 유지하면서 `error reply` metadata를 설정한다.
5. application part 배열을 새 배열로 합치는 `combined` stack/heap 경로를 제거한다.
6. multipart part-wise API는 final submit 시 완성된 첫 part에 metadata가 정확히 붙도록 한다.
7. metadata는 가능한 한 Core가 이미 소유한 staged/moved message에만 설정한다.
8. 호출자 소유 message에 metadata를 설정해야 한다면 RAII rollback guard를 사용해
   ownership이 호출자에게 남는 모든 실패에서 kind와 sequence를 초기화한다.
9. immediate single-part, staged multipart, complete-array, routed/non-routed 경로의
   metadata 설정 위치를 각각 검증한다.
10. allocation failure, backpressure와 send 실패에서 원본 보존·metadata rollback·ownership
    계약을 확인한다.

주요 수정 후보:

- `core/src/api/socket/request_reply_protocol_internal.hpp`
- `core/src/api/socket/socket_request_reply_submit_api.cpp`
- `core/src/api/socket/socket_request_reply_runtime_io.cpp`
- `core/src/api/socket/socket_request_reply_submit_internal.hpp`

완료 조건: request와 reply 송신이 application part 수를 늘리지 않으며, envelope용
`zlink_msg_t`와 `std::vector`를 만들지 않는다. submit 실패 후 호출자에게 남은 message는
`kind=data`, `request_seq=0`이며 ordinary send에 다시 사용할 수 있다.

### Phase 4. 수신·dispatch 경로 전환

1. `parse_envelope(parts, count)`의 content sniffing을 제거한다.
2. 첫 part의 ZMP 내부 metadata로 raw/request/reply/error를 구분한다.
3. request 전용 recv와 handler에 sequence를 전달한 뒤 public payload에서는 내부 metadata를
   제거한다.
4. Completion lane은 reply metadata의 sequence로 pending entry를 찾는다.
5. 일반 receive로 내보낸 payload가 protocol metadata를 public property로 노출하지 않게 한다.
6. raw multipart가 과거 envelope byte 패턴과 같아도 raw payload로 전달되는 회귀 test를
   추가한다.
7. receive buffer inline capacity를 application payload 기준으로 다시 계산한다. 기존
   `4 control + 1~2 payload`라는 주석과 상수를 제거한다.

주요 수정 후보:

- `core/src/api/socket/socket_request_reply_runtime_io.cpp`
- `core/src/api/socket/socket_request_reply_dispatch.cpp`
- `core/src/api/socket/socket_request_reply_router_control.cpp`
- `core/src/api/socket/request_reply_frame_buffer_internal.hpp`

완료 조건: socket layer에 protocol id/version/type/sequence payload parser가 남지 않고,
`request_seq`는 ZMP decoder가 복원한 metadata에서만 온다.

### Phase 5. Contract test와 malformed wire test

다음 순서로 가장 작은 test부터 확장한다.

1. `unittest_zmp_decoder`
   - data 8바이트 header
   - request/reply/error 16바이트 header
   - zero/nonzero sequence
   - 잘린 extension
   - 알 수 없는 kind
   - 금지 flag 조합
   - multipart 중간 metadata
   - identity/control 뒤 첫 application frame
   - multipart 진행 중 identity/control 거부
   - 마지막 frame 뒤 multipart 상태 초기화
   - extension 검증 실패 시 reservation 해제
   - backpressure retry 뒤 kind/sequence 보존과 admission 1회 성공
2. `unittest_zmp_contract_edges`
   - payload size가 extension을 제외하는지 확인
   - 최대 payload와 overflow
   - encoder가 invalid metadata를 거부하는지 확인
   - generic encoder, Asio ZMP와 WS gather header의 동일 byte 결과
3. `test_zmp_request_reply`
   - DEALER→ROUTER request/reply
   - ROUTER→ROUTER request/reply
   - single/multipart/empty payload
   - timeout, duplicate reply, error reply, disconnect
   - callback과 direct recv 표면
   - immediate single-part와 staged part-wise submit
   - complete-array와 routed/non-routed submit
   - allocation/backpressure 실패 후 metadata rollback과 message 재사용
4. `test_zmp_request_reply_router_recv_surface`
   - payload part count 보존
   - raw send와 request 판별
5. `test_zmp_ws_wss`
   - TCP와 같은 metadata 배치
   - WebSocket binary message 경계
6. raw wire fixture
   - 첫 네 application parts가 옛 envelope와 완전히 같아도 ordinary send이면 raw로 전달
   - request payload가 옛 signature로 시작해도 손상되지 않음

기존 raw request helper가 4개 envelope part를 직접 만들면 새 ZMP header fixture로
교체한다. test expectation을 낮춰 통과시키지 않는다.

### Phase 6. 명세와 설명 문서

보호 문서 승인을 받은 경우에만 다음을 수정한다.

`core/doc/spec/core/protocol/01-zmp.ko.md`:

- 4-part request-reply envelope 절을 제거한다.
- ZMP header의 byte 3과 optional sequence extension을 정의한다.
- multipart 첫 frame 규칙과 malformed 조합을 정의한다.
- payload part 수 보존과 content sniffing 금지를 검증 요구에 넣는다.
- TCP/IPC/WS/WSS에서 동일한 byte 배치를 명시한다.

`bindings/doc/spec/README.ko.md`:

- bindings가 4-part envelope를 인지한다는 설명을 제거한다.
- bindings는 Core request/reply API에 payload만 전달하고 wire metadata를 재구현하지
  않는다고 적는다.

일반 guide가 현재 4-part 배치를 설명한다면 함께 갱신한다. 공개 문서에서 이 plan으로
연결하는 링크는 만들지 않는다.

완료 조건: source, formal spec, wire fixture가 동일한 byte 배치를 설명하고, 공개 문서에
plan 링크가 없다.

### Phase 7. GitHub Actions 검증과 성능 확인

Core 로컬 build는 하지 않는다. 사용자가 commit/push를 지시한 뒤 해당 commit SHA를
대상으로 `.github/workflows/build.yml`의 `workflow_dispatch`를 실행한다. Linux x64 job은
Core test를 포함해 실행하고 `libzlink-linux-x64` artifact를 생성한다. 검증 기록에는
workflow run ID, branch, commit SHA와 artifact 이름을 남긴다. 다른 commit의 artifact나
release asset을 대신 사용하지 않는다.

기능 gate:

- Core build 성공
- ZMP unit test 성공
- request-reply integration test 성공
- TCP, IPC, WS, WSS contract 성공
- sanitizer나 protocol test workflow가 있으면 함께 성공

성능 gate는 C Multi release build로 실행한다.

고정 조건:

- clients: 100
- I/O threads: 4
- payload size: 1024
- application part count: 2 (`payload + empty`)
- transport: TCP, WSS
- 반복: 최종 비교 5회
- 기준: 변경 전 동일 official Core artifact

먼저 다음 네 case를 비교한다.

- DEALER_ROUTER_SENDSEND
- DEALER_ROUTER_REQREP
- ROUTER_ROUTER_SENDSEND
- ROUTER_ROUTER_REQREP

그 뒤 사용자 요청에 따라 모든 C Multi pattern을 TCP/WSS, 1024바이트 smoke로 한 번
실행한다. perf 코드를 바꾸어 성능을 맞추지 않는다.

반드시 기록할 값:

- 각 case의 5회 개별 throughput과 중앙값
- latency 중앙값과 tail
- REQREP/SENDSEND throughput 비율
- 변경 전/후 REQREP 변화율
- 성공 case 수와 timeout/error 수
- 사용한 Core artifact revision

변경 전 C baseline 참고:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260830_022104_final-0146-c-baseline-r5.txt`

| Pattern | TCP | WSS |
|---|---:|---:|
| DEALER_ROUTER_SENDSEND | 207.532 Kmsg/s | 142.971 Kmsg/s |
| DEALER_ROUTER_REQREP | 82.207 Kmsg/s | 85.940 Kmsg/s |
| REQREP/SENDSEND | 39.6% | 60.1% |
| ROUTER_ROUTER_SENDSEND | 159.280 Kmsg/s | 165.580 Kmsg/s |
| ROUTER_ROUTER_REQREP | 91.540 Kmsg/s | 86.620 Kmsg/s |
| REQREP/SENDSEND | 57.5% | 52.3% |

frame 수 감소만으로 특정 처리량을 보장하지 않는다. pending map, timeout scheduler와
completion callback 비용은 남는다. 다만 wire 확인에서 SENDSEND와 REQREP의 application
frame 수가 같아야 하고, REQREP 처리량이 악화되면 원인을 설명하기 전에는 완료하지 않는다.

## 7. 완료 조건

다음 항목을 모두 만족해야 작업을 완료로 본다.

- [ ] request/reply/error kind와 sequence가 ZMP metadata로 전달된다.
- [ ] request-reply 송신이 application envelope part를 생성하지 않는다.
- [ ] 일반 send와 request가 같은 application payload part 수를 유지한다.
- [ ] payload byte 패턴으로 request 여부를 판별하는 코드가 없다.
- [ ] 옛 envelope와 같은 raw multipart가 raw message로 전달된다.
- [ ] auxiliary storage에 별도 discriminator가 있고 request metadata를 long group으로
      해석하는 경로가 없다.
- [ ] long group과 request metadata의 init/copy/move/close test가 통과한다.
- [ ] `sizeof(msg_t) == 64`, `max_vsm_size == 29`가 유지된다.
- [ ] generic encoder, Asio ZMP와 WS engine이 공통 data header builder를 사용한다.
- [ ] extension 검증 뒤 application payload 크기로 admission하며 실패 시 reservation을
      남기지 않는다.
- [ ] multipart·identity·control 상태 전이와 backpressure admission retry test가 통과한다.
- [ ] submit 실패로 호출자에게 남은 message의 protocol metadata가 초기화된다.
- [ ] 일반 data ZMP header와 hot path에 동적 allocation이 추가되지 않는다.
- [ ] pending, timeout, duplicate reply와 completion 의미가 유지된다.
- [ ] TCP, IPC, WS, WSS contract test가 성공한다.
- [ ] 보호 문서 승인을 받은 범위에서 formal spec이 구현과 일치한다.
- [ ] C Multi TCP/WSS 1024바이트 비교 결과와 Core revision이 기록된다.
- [ ] 모든 C Multi pattern TCP/WSS 1024바이트 smoke가 완료된다.
- [ ] `git diff --check`가 통과한다.
- [ ] 요청 범위 밖의 기존 변경이 수정되거나 commit에 섞이지 않는다.

## 8. 중단하고 보고해야 하는 조건

다음 중 하나가 발생하면 우회 구현을 넣지 말고 사용자에게 보고한다.

- `msg_t`를 64바이트보다 키워야만 구현할 수 있다.
- VSM 한계를 29바이트보다 낮춰야 한다.
- auxiliary storage가 group과 request metadata를 구분할 discriminator를 16바이트 안에
  둘 수 없다.
- 일반 data frame에도 request 전용 allocation·map·mutex 비용이 생긴다.
- ZMP data header 생성 경로를 공통 helper로 합칠 수 없어 engine별 byte 배치가 중복된다.
- ZMP transport별로 서로 다른 request metadata 배치가 필요하다.
- Framework metadata codec을 Core가 참조해야 한다.
- public message metadata API가 필요해진다.
- compatibility fallback이나 제품 version 상승이 필요하다는 새 요구가 나온다.
- protected spec을 수정해야 하지만 경로 승인이 없다.
- GitHub Actions artifact가 아닌 로컬 Core build가 필요하다.
- 기존 pending/completion 계약을 바꿔야만 wire 변경이 가능하다.

## 9. 별도 세션 시작용 작업 지시문

다음 문장을 새 세션의 첫 요청으로 사용할 수 있다.

> `doc/plan/zmp-request-reply-protocol-metadata-plan.ko.md`를 처음부터 끝까지 읽고
> D1~D9를 고정 조건으로 실행해줘. 현재 worktree의 기존 bindings 성능 변경은 보존하고
> 섞지 마. request/reply kind와 request_seq를 application 4-part envelope에서 ZMP
> header metadata로 옮기되 application payload part 수와 byte는 일반 send와 동일하게
> 유지해. `sizeof(msg_t)=64`, `max_vsm_size=29`를 바꾸지 말고, Core 로컬 build는 하지
> 마. 보호 문서는 수정 전에 정확한 경로와 범위를 확인해.
