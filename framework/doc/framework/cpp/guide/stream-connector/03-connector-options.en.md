# 03 — Connector Options

[← Getting Started](02-getting-started.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Sending Packets →](04-sending.en.md)

---

`connector_options_t` is the settings structure passed when creating a connector.

```cpp
zlink::stream_connector::connector_options_t options;
auto connector = zlink::stream_connector::connector_factory_t::create(options);
```

## endpoint

```cpp
options.endpoint = "tcp://game.example.com:7000";
options.endpoint = "wss://game.example.com:443/stream";
```

The scheme decides the transport. The supported schemes are `tcp`, `tls`, `ws`, `wss`.

| scheme | transport | required build feature |
|--------|-----------|--------------------|
| `tcp://host:port` | TCP | always included |
| `tls://host:port` | TLS over TCP | `WITH_TLS` |
| `ws://host:port/path` | WebSocket | `WITH_WEBSOCKET` |
| `wss://host:port/path` | WebSocket over TLS | `WITH_WEBSOCKET` + `WITH_TLS` |

Using a transport that isn't in the build doesn't return an `unsupported_codec` error from
`connect()` — it currently returns `configuration_error`.

## transport

Automatically set from the endpoint scheme. It can also be specified separately from the scheme.

```cpp
options.transport = zlink::stream_connector::transport_t::websocket_secure;
```

## timeout

```cpp
options.connect_timeout   = std::chrono::seconds{5};   // the overall limit for a connect attempt
options.request_timeout   = std::chrono::seconds{30};  // the default limit for a request
options.wait_timeout      = std::chrono::seconds{5};   // the default limit for wait_for
```

`request_timeout` is the default for `request().submit()`, and `wait_timeout` is the default for
`wait_for().submit()`. Either can be overridden per call with `.timeout()`.

## heartbeat

```cpp
options.heartbeat.enabled  = true;
options.heartbeat.interval = std::chrono::seconds{10}; // the ping interval while idle
options.heartbeat.timeout  = std::chrono::seconds{30}; // disconnected if no response within this time
```

Heartbeat uses the `$zlink.heartbeat.ping` / `$zlink.heartbeat.pong` control frames. These frames
aren't delivered to an `on<packet_t>()` callback.

## reconnect

```cpp
options.reconnect.enabled       = true;
options.reconnect.initial_delay = std::chrono::milliseconds{250};
options.reconnect.max_delay     = std::chrono::seconds{5};
options.reconnect.backoff_factor = 2.0;
options.reconnect.max_attempts  = 3; // unlimited if std::nullopt
```

Both a first-connection failure and a connection drop trigger a reconnect attempt. A `reconnecting`
status event is published while retrying. If every attempt fails, it transitions to `disconnected`.

## dispatch_mode

```cpp
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;    // default
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
```

- `manual`: the `on<T>()` callback runs when `dispatch()` is called. Use this to align with a game
  engine's frame loop.
- `immediate`: the callback runs immediately on the connector's receive path. Suits a CLI, tool, or
  e2e client.

See [05 — Receiving Packets](05-receiving.en.md) for details.

## compression

```cpp
options.compression = zlink::stream_connector::compression_t::lz4; // default
options.compression_codec = zlink::stream_connector::lz4_compression_codec();
```

The codec setting the connector uses when sending and receiving a compressed frame. The default is
LZ4. This default doesn't mean every frame is automatically compressed — only a frame where
`.compress()` requested per-packet compression per call gets compressed.

The server framework and connector must use the same compression codec. Even when using a custom
codec, configure it through the same option path as the built-in LZ4.

```cpp
options.compression = zlink::stream_connector::compression_t::lz4;
options.compression_codec = std::make_shared<my_compression_codec_t>();
```

If compression is explicitly turned off, a send/request that called `.compress()` fails at the send
stage, and receiving a compressed frame is treated as a restore error at the receive stage.

```cpp
options.compression = zlink::stream_connector::compression_t::none;
options.compression_codec.reset();
```

## Payload/Metadata Size Limits

```cpp
options.max_send_payload_size    = 64 * 1024; // default 64 KB
options.max_receive_payload_size = 64 * 1024; // default 64 KB
options.max_metadata_size        = 8 * 1024;  // default 8 KB
```

`max_send_payload_size` limits the payload size `send()` and `request()` can send. Exceeding this
limit returns a `frame_too_large` error before the transport write.

`max_receive_payload_size` limits the encoded payload size of a stream frame received from the
server. A frame exceeding this limit is rejected with a `frame_too_large` error before a payload
buffer is built. If the server can send a larger push or reply, raise this value explicitly.

`max_metadata_size` applies to both the send metadata and the receive frame header size. If your
protocol uses large metadata, adjust it separately from the payload cap.

## TLS Certificate Verification

```cpp
options.skip_server_certificate_validation = false; // default (verified)
```

Set this to `true` only when using a self-signed certificate in a development environment. Don't
use it in production.

## Full Example

```cpp
namespace zsc = zlink::stream_connector;

zsc::connector_options_t options;
options.endpoint                 = "wss://game.example.com:443/stream";
options.connect_timeout          = std::chrono::seconds{5};
options.request_timeout          = std::chrono::seconds{10};
options.wait_timeout             = std::chrono::seconds{5};
options.heartbeat.interval       = std::chrono::seconds{15};
options.heartbeat.timeout        = std::chrono::seconds{45};
options.reconnect.max_attempts   = 5;
options.reconnect.max_delay      = std::chrono::seconds{10};
options.dispatch_mode            = zsc::dispatch_mode_t::manual;

auto connector = zsc::connector_factory_t::create(options);
```
