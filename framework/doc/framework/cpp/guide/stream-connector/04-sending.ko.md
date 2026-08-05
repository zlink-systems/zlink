# 04 — 패킷 송신

[← Connector 옵션](03-connector-options.ko.md) | [목차](INDEX.ko.md) | [다음: 패킷 수신 →](05-receiving.ko.md)

---

## send — 단방향 송신

`send()`는 reply를 기다리지 않는 단방향 패킷을 보낸다.

```cpp
struct position_update_t {
    float x, y, z;
    float timestamp;
};

auto result = connector
    .send(position_update_t{102.5f, 0.0f, -44.3f, 1718500000.0f})
    .packet_name("player.position")
    .submit();

if (!result) {
    // result.error_code()
}
```

callback 방식:

```cpp
connector
    .send(position_update_t{102.5f, 0.0f, -44.3f, 1718500000.0f})
    .packet_name("player.position")
    .submit([](zlink::stream_connector::result_t<void> result) {
        if (!result) {
            // send 실패 처리
        }
    });
```

## request — 요청/응답

`request(...)`는 서버 응답을 기다린다. 응답 타입은 `submit<TReply>()`에서 선언한다. reply는 sequence로 매칭되므로 동시에 여러 request를 보내도 순서와 무관하게 각각 완료된다.

```cpp
struct match_join_request_t {
    std::string match_id;
    std::string player_id;
};

struct match_join_reply_t {
    int32_t slot;
    std::string team;
    int32_t player_count;
};

auto reply = connector
    .request(match_join_request_t{"match-7f3a", "player-1"})
    .packet_name("match.join")
    .timeout(std::chrono::seconds{5})
    .submit<match_join_reply_t>();

if (!reply) {
    if (reply.error_code() == zlink::stream_connector::error_code_t::request_timeout) {
        // 서버 응답 없음
    }
    return;
}

auto slot = reply.value().slot;
```

callback 방식:

```cpp
connector
    .request(match_join_request_t{"match-7f3a", "player-1"})
    .packet_name("match.join")
    .submit<match_join_reply_t>([](zlink::stream_connector::result_t<match_join_reply_t> result) {
        if (!result) { return; }
        // result.value().slot
    });
```

## packet_name 결정 우선순위

1. `.packet_name("이름")`으로 명시한 경우 그 값을 사용한다.
2. 없으면 DTO의 `static constexpr const char* packet_name`을 사용한다.
3. 없으면 C++ type name fallback을 사용한다.

```cpp
struct inventory_update_t {
    static constexpr const char* packet_name = "inventory.update";
    // ...
};

// packet_name 생략 시 "inventory.update"가 자동 사용됨
connector.send(inventory_update_t{}).submit();
```

## metadata

packet에 키-값 메타데이터를 붙인다. 라우팅, 추적, 버전 정보 등에 사용한다.

```cpp
connector
    .send(chat_message_t{"room-42", "안녕하세요"})
    .packet_name("chat.send")
    .metadata("x-locale", "ko-KR")
    .metadata("x-client-version", "2.4.1")
    .submit();
```

`metadata_t` 객체로 한 번에 설정할 수도 있다.

```cpp
zlink::stream_connector::metadata_t meta;
meta.with("x-locale", "ko-KR").with("x-client-version", "2.4.1");

connector.send(msg).metadata(std::move(meta)).submit();
```

## 압축

패킷 단위로 LZ4 압축을 요청한다. 압축 feature가 build에 없으면 이 호출은 무시된다.

```cpp
connector
    .send(large_map_chunk_t{chunk_data})
    .packet_name("world.chunk")
    .compress()
    .submit();
```

## codec

기본 codec은 JSON이다. raw bytes를 직접 보내야 하는 경우에만 raw packet을 직접 조립한다.
MessagePack이나 Protobuf는 stream connector 전용 feature가 아니라 framework codec extension으로 등록한다.

```cpp
connector
    .send(inventory_update_t{})
    .submit();
```

codec_t 값:

| 값 | 의미 |
|----|------|
| `raw` | raw bytes. payload 필드에 직접 넣어야 함 |
| `json` | JSON (기본값) |
| `message_pack` | framework MessagePack codec extension이 등록한 패킷 |
| `protobuf` | framework Protobuf codec extension이 등록한 패킷 |

## raw packet 직접 조립

DTO 없이 `packet_t`를 직접 만들 수 있다.

```cpp
zlink::stream_connector::packet_t packet;
packet.name    = "debug.ping";
packet.payload = {0x01, 0x02, 0x03};
packet.codec   = zlink::stream_connector::codec_t::raw;

connector.send(std::move(packet)).submit();
```

## 크기 제한

`max_send_payload_size`(기본 64 KB)와 `max_metadata_size`(기본 8 KB)를 넘으면
`frame_too_large` 오류를 반환한다. 오류가 반환되는 시점은 transport write 전이다.

서버에서 받는 push와 reply payload에는 `max_receive_payload_size`(기본 64 KB)가 적용된다. 큰
payload를 받을 수 있는 connector는 옵션에서 이 값을 명시적으로 올린다.
