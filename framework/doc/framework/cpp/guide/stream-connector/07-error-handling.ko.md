# 07 — 오류 처리

[← 연결 생명주기](06-lifecycle.ko.md) | [목차](INDEX.ko.md) | [다음: E2E 클라이언트 →](08-e2e-client.ko.md)

---

## result_t\<T\>

모든 동기 API는 `result_t<T>`를 반환한다. exception을 던지지 않는다.

```cpp
auto reply = connector.request(request).submit<match_join_reply_t>();

if (!reply) {
    // 실패
    auto code = reply.error_code();
    auto msg  = reply.error() ? reply.error()->message : "";
    return;
}

auto value = reply.value(); // T&&
```

| 표현식 | 의미 |
|--------|------|
| `if (result)` | 성공 여부 확인 |
| `result.value()` | 성공 값 (실패 상태에서 호출하면 UB) |
| `result.error_code()` | `error_code_t` 열거값 |
| `result.error()` | `const error_t*`. 메시지 포함. 성공 시 nullptr |

## error_code_t 목록

| 코드 | 의미 | 주요 발생 API |
|------|------|--------------|
| `disconnected` | 연결이 없는 상태에서 operation 호출 또는 transport 끊김 | send, request, wait, dispatch |
| `configuration_error` | endpoint, packet name, timeout 같은 설정이 잘못됨 | connect, send, request |
| `validation_failed` | 요청 인자가 계약 범위를 벗어남 | send, request |
| `request_timeout` / `wait_timeout` | reply나 wait 대상 packet이 timeout 안에 도착하지 않음 | request, wait_for |
| `connect_timeout` | connect 시도가 `connect_timeout` 안에 완료되지 않음 | connect |
| `frame_decode_failed` | 수신 frame을 STREAM 계약에 맞게 파싱할 수 없음 | receive loop |
| `frame_too_large` | send payload 또는 metadata가 설정 한도를 넘음 | send, request |
| `send_failed` | 연결은 열려 있지만 packet write가 실패함 | send, request |
| `unsupported_codec` | build에 없는 codec을 사용함 | send, request |
| `compression_failed` | 설정된 compression codec으로 payload를 압축할 수 없음 | send, request |
| `tls_validation_failed` | TLS 서버 인증서 검증 실패 | connect (TLS/WSS) |
| `decompression_failed` | 설정된 compression codec으로 compressed payload를 복원할 수 없음 | receive loop |
| `user_callback_failed` | `on<T>()` callback 안에서 예외가 발생함 | dispatch |
| `remote_error` | 서버가 error frame을 응답함 | request |
| `closed` | `close()`를 호출해 pending operation이 종료됨 | 모든 pending operation |
| `canceled` | coroutine task 파괴나 명시 취소로 operation이 종료됨 | e2e client awaiter |

## 패턴별 처리

### timeout 재시도

```cpp
auto reply = connector
    .request(query)
    .timeout(std::chrono::seconds{5})
    .submit<match_data_t>();

if (!reply && reply.error_code() == zsc::error_code_t::request_timeout) {
    // 재시도 또는 fallback
}
```

### disconnected 처리

`send()`나 `request()`가 `disconnected`를 반환하면 reconnect가 진행 중이거나 이미 실패한 상태다. 상태 이벤트를 구독해 reconnect 완료 후 재시도한다.

```cpp
connector.on_connection_state_changed([&](const zsc::connection_state_changed_t& ev) {
    if (ev.state == zsc::connection_state_t::connected) {
        // reconnect 성공 후 pending 작업 재시도
    }
});
```

### remote_error

서버가 error frame을 돌려보낼 때 발생한다.

```cpp
if (!reply && reply.error_code() == zsc::error_code_t::remote_error) {
    auto msg = reply.error() ? reply.error()->message : "unknown";
    // msg: 서버에서 보낸 오류 메시지
}
```

### frame_too_large 예방

```cpp
// payload가 클 수 있는 경우 compress() 사용
connector
    .send(large_payload_t{data})
    .compress()
    .submit();

// 또는 options에서 한도 올리기
options.max_send_payload_size = 512 * 1024;
options.max_receive_payload_size = 512 * 1024;
```

compression을 명시적으로 끈 상태에서 `.compress()`를 호출하면 `compression_failed`가
난다. 같은 상태에서 compressed frame을 받으면 `decompression_failed`가 나며, 오류 메시지는
compression codec이 설정되지 않았다는 뜻을 드러낸다. 압축을 풀어 나온 payload도
`max_receive_payload_size`를 다시 넘으면 `frame_too_large`로 처리된다.

## throwing adapter

`result_t<T>` 대신 예외를 던지는 helper가 필요하다면 `zlink/stream_connector_throwing.hpp`를 사용한다.

```cpp
#include <zlink/stream_connector_throwing.hpp>

// 성공하면 TReply를 반환, 실패하면 zlink::stream_connector::stream_error 예외
auto reply = zlink::stream_connector_throwing::request<match_join_reply_t>(
    connector, request);
```

throwing adapter는 server framework나 tool 코드를 위한 선택 표면이다. 게임 엔진 client에서는 core `result_t<T>` API를 직접 사용한다.

## 오류 없이 성공만 진행하는 pattern

```cpp
auto connected = connector.connect();
if (!connected) { return; }

auto auth = connector
    .request(auth_request_t{"player-1", "tok-abc123"})
    .submit<auth_reply_t>();
if (!auth) { return; }

connector
    .send(enter_world_t{auth.value().session_id, "zone-12"})
    .submit();
```
