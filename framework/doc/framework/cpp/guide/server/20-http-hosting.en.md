---
title: "20. HTTP Hosting · C++"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.en.md) | [Previous: 19. Configuration](19-configuration.en.md) | [Next: 4. Backpressure](04-backpressure.en.md)
<!-- framework-adapter-nav:end -->

# 20. HTTP Hosting

> **The document that owns this chapter's contract** — covered by
> [C++ HTTP hosting public contract](../../../common/spec/server/languages/cpp/60-http-hosting.en.md).
> This chapter explains how to open an embedded HTTP server. The sending side is a separate
> artifact — see the HTTP Client guide.

## 1. What The Embedded HTTP Server Does

Opens an HTTP endpoint inside the framework app. It's the entry point for external systems
and web clients coming in over REST, and the handler model is the same as a channel's --
just map the same handler class onto an HTTP route.

The **sending** side (the client) is a separate artifact --
the [zlink::http_client guide](../http-client/README.ko.md).

## 2. Mapping Routes

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .map_post<create_game_http_handler_t> ("/games")
  .map_get<get_game_http_handler_t> ("/games/{gameId}")
  .map_put<update_settings_http_handler_t> ("/games/{gameId}/settings")
  .map_delete<cancel_game_http_handler_t> ("/games/{gameId}");
```

The handler is the same common model as [Chapter 2 §3](02-getting-started.en.md). DTO
serialization on the HTTP path is JSON by default, using the DTO's `to_json`/`from_json`
ADL functions. Route registration auto-registers the JSON serializer for the
request/reply types, and doesn't overwrite a type already registered in that same
serializer registry.

- A route parameter uses `{name}` syntax. `/games/{gameId}` matches
  `/games/g-20260611-0042`. In a raw HTTP handler, the URL-decoded value arrives via
  `http_request_t::route_values`; in a typed DTO handler, it's merged with the
  body/query values into the `request_type` deserialization input.
- Mapping the same method+path twice is rejected at configuration time.

Both synchronous and coroutine handlers work. The typical pattern of receiving an HTTP
request and delegating it to a channel is in [Chapter 5 §3](05-channel-messaging.en.md).

## 3. Health Endpoint

The health endpoint is only exposed if you **explicitly map it.**

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .map_health ("/healthz")
  .map_readiness ("/ready")
  .map_liveness ("/live");
```

The response is JSON with `status`, `readiness`, `liveness`, and `checks` fields. The
checks that make up the status are registered via `app.health()` in chapter `11.
Monitoring`.

```bash
$ curl -s http://127.0.0.1:8080/ready
{"status":"healthy","readiness":"healthy","liveness":"healthy","checks":[]}
```

## 4. Middleware

Common processing in front of routes (auth, logging, etc.) is inserted as middleware.

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .use<bearer_auth_middleware_t> ()
  .map_post<create_game_http_handler_t> ("/games");
```

## 5. TLS

An `https://` endpoint gets its certificate from `configure_tls` right after `listen`.
`configure_tls` applies to **the most recently declared listen endpoint.**

```cpp
options.http ()
  .listen ("https://0.0.0.0:8443")
  .configure_tls ([] (zlink::framework::http_tls_options_builder_t &tls) {
      tls.certificate_file ("/etc/pki/game-api.crt.pem")
         .private_key_file ("/etc/pki/game-api.key.pem");
  });
```

Calling `configure_tls` without a preceding `listen` is rejected as a configuration error.

## 6. Server Options

Operational limits are adjusted with `configure_server`.

```cpp
options.http ().configure_server ([] (zlink::framework::http_server_options_builder_t &server) {
    server.set_max_connections (10000)
      .set_max_request_body_size (1 * 1024 * 1024)
      .set_request_headers_timeout (std::chrono::seconds (5))
      .set_keep_alive_timeout (std::chrono::seconds (60))
      .set_max_keep_alive_requests (100)
      .set_graceful_shutdown_timeout (std::chrono::seconds (10));
});
```

| Option | Meaning | Default |
|------|------|--------|
| `set_max_connections(n)` | Concurrent connection cap -- connections over this are rejected by the runtime overload policy | 1024 |
| `set_max_request_body_size(bytes)` / `set_max_header_size(bytes)` | Request size cap -- 413/431 if exceeded | 1MB / 64KB |
| `set_request_headers_timeout(ms)` / `set_request_body_timeout(ms)` | Per-stage receive timeout | 5000ms / 5000ms |
| `set_write_timeout(ms)` | Response write timeout | 5000ms |
| `set_keep_alive_timeout(ms)` / `set_max_keep_alive_requests(n)` | Keep-alive connection retention policy | 5000ms / 100 |
| `set_graceful_shutdown_timeout(ms)` | How long to wait for in-progress requests to finish on shutdown | 5000ms |

On shutdown, the server first blocks new connections and waits for in-progress requests to
finish. Connections not done within the timeout, or ones waiting on keep-alive, are cleaned
up, so `stop()` never hangs.

## 7. Related Documents

- The formal contract: [C++ HTTP hosting public contract](../../../common/spec/server/languages/cpp/60-http-hosting.en.md)
- The handler model: [13. Key Type Usage Index](13-interface-catalog.en.md)
- The health endpoint: [11. Monitoring](11-monitoring.en.md)
