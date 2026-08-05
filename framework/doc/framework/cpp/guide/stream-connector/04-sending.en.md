# 04 — Sending Packets

[← Connector Options](03-connector-options.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Receiving Packets →](05-receiving.en.md)

---

## send — One-Way Sending

`send()` sends a one-way packet with no reply expected.

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

Callback style:

```cpp
connector
    .send(position_update_t{102.5f, 0.0f, -44.3f, 1718500000.0f})
    .packet_name("player.position")
    .submit([](zlink::stream_connector::result_t<void> result) {
        if (!result) {
            // handle the send failure
        }
    });
```

## request — Request/Reply

`request(...)` waits for a server response. The response type is declared in `submit<TReply>()`.
Since replies are matched by sequence, sending multiple requests at the same time still completes
each one correctly, regardless of order.

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
        // no server response
    }
    return;
}

auto slot = reply.value().slot;
```

Callback style:

```cpp
connector
    .request(match_join_request_t{"match-7f3a", "player-1"})
    .packet_name("match.join")
    .submit<match_join_reply_t>([](zlink::stream_connector::result_t<match_join_reply_t> result) {
        if (!result) { return; }
        // result.value().slot
    });
```

## packet_name Resolution Priority

1. If specified via `.packet_name("name")`, that value is used.
2. Otherwise, the DTO's `static constexpr const char* packet_name` is used.
3. Otherwise, a C++ type-name fallback is used.

```cpp
struct inventory_update_t {
    static constexpr const char* packet_name = "inventory.update";
    // ...
};

// If packet_name is omitted, "inventory.update" is used automatically
connector.send(inventory_update_t{}).submit();
```

## metadata

Attaches key-value metadata to a packet. Used for routing, tracing, version info, and the like.

```cpp
connector
    .send(chat_message_t{"room-42", "안녕하세요"})
    .packet_name("chat.send")
    .metadata("x-locale", "ko-KR")
    .metadata("x-client-version", "2.4.1")
    .submit();
```

It can also be set all at once with a `metadata_t` object.

```cpp
zlink::stream_connector::metadata_t meta;
meta.with("x-locale", "ko-KR").with("x-client-version", "2.4.1");

connector.send(msg).metadata(std::move(meta)).submit();
```

## Compression

Requests LZ4 compression per packet. If the compression feature isn't in the build, this call is
ignored.

```cpp
connector
    .send(large_map_chunk_t{chunk_data})
    .packet_name("world.chunk")
    .compress()
    .submit();
```

## codec

The default codec is JSON. Assemble a raw packet directly only when you need to send raw bytes
directly. MessagePack and Protobuf aren't stream-connector-specific features — they're registered as
framework codec extensions.

```cpp
connector
    .send(inventory_update_t{})
    .submit();
```

`codec_t` values:

| Value | Meaning |
|----|------|
| `raw` | raw bytes. Must be put directly into the payload field |
| `json` | JSON (default) |
| `message_pack` | a packet registered by the framework MessagePack codec extension |
| `protobuf` | a packet registered by the framework Protobuf codec extension |

## Assembling A Raw Packet Directly

You can build a `packet_t` directly, without a DTO.

```cpp
zlink::stream_connector::packet_t packet;
packet.name    = "debug.ping";
packet.payload = {0x01, 0x02, 0x03};
packet.codec   = zlink::stream_connector::codec_t::raw;

connector.send(std::move(packet)).submit();
```

## Size Limits

Exceeding `max_send_payload_size` (default 64 KB) and `max_metadata_size` (default 8 KB) returns a
`frame_too_large` error. This error is returned before the transport write.

`max_receive_payload_size` (default 64 KB) applies to a push and reply payload received from the
server. A connector that can receive a large payload raises this value explicitly in the options.
