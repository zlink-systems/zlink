---
title: "20. HTTP Hosting · C++"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 19. Configuration](19-configuration.ko.md) | [다음: 4. Backpressure](04-backpressure.ko.md)
<!-- framework-adapter-nav:end -->

# 20. HTTP Hosting

> **이 장의 계약 소유 문서** — [C++ HTTP hosting 공개 계약](../../../common/spec/server/languages/cpp/60-http-hosting.ko.md)이
> 다룬다. 이 챕터는 embedded HTTP server를 여는 방법을 설명한다. 요청을 보내는 쪽은
> 별도 산출물이다 — HTTP Client 가이드를 본다.

## 1. embedded HTTP server가 하는 일

프레임워크 앱 안에 HTTP endpoint를 연다. 외부 시스템·웹 클라이언트가 REST로
들어오는 입구이며, 핸들러 모델은 채널과 동일하다 — 같은 핸들러 클래스를 HTTP
라우트에 매핑만 하면 된다.

요청을 **보내는** 쪽(client)은 별도 산출물이다 —
[zlink::http_client 가이드](../http-client/README.ko.md).

## 2. 라우트 매핑

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .map_post<create_game_http_handler_t> ("/games")
  .map_get<get_game_http_handler_t> ("/games/{gameId}")
  .map_put<update_settings_http_handler_t> ("/games/{gameId}/settings")
  .map_delete<cancel_game_http_handler_t> ("/games/{gameId}");
```

핸들러는 [2장 §3](02-getting-started.ko.md)의 공통 모델 그대로다. HTTP 경로의
DTO 직렬화는 기본으로 JSON이며 DTO의 `to_json`/`from_json` ADL 함수를 쓴다.
route 등록 시 request/reply 타입의 JSON serializer를 자동 등록하고, 같은 serializer
registry에 이미 등록된 타입은 덮어쓰지 않는다.

- 경로 파라미터는 `{name}` 문법이다. `/games/{gameId}`는 `/games/g-20260611-0042`에
  매칭된다. raw HTTP 핸들러에서는 URL-decode된 값이 `http_request_t::route_values`로
  들어오고, typed DTO 핸들러에서는 body/query 값과 함께 `request_type` 역직렬화 입력에
  합쳐진다.
- 같은 메서드+경로를 두 번 매핑하면 구성 시점에 거부된다.

동기 핸들러와 코루틴 핸들러 모두 가능하다. HTTP 요청을 받아 채널로 위임하는
전형적인 패턴은 [5장 §3](05-channel-messaging.ko.md)에 있다.

## 3. health endpoint

health endpoint는 **명시적으로 매핑해야** 노출된다.

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .map_health ("/healthz")
  .map_readiness ("/ready")
  .map_liveness ("/live");
```

응답은 `status`, `readiness`, `liveness`, `checks` 필드를 가진 JSON이다. 상태를 구성하는 check는
`11. Monitoring` 장의 `app.health()`로 등록한다.

```bash
$ curl -s http://127.0.0.1:8080/ready
{"status":"healthy","readiness":"healthy","liveness":"healthy","checks":[]}
```

## 4. middleware

라우트 앞단의 공통 처리(인증, 로깅 등)는 middleware로 끼운다.

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .use<bearer_auth_middleware_t> ()
  .map_post<create_game_http_handler_t> ("/games");
```

## 5. TLS

`https://` endpoint는 `listen` 직후 `configure_tls`로 인증서를 준다.
`configure_tls`는 **마지막에 선언한 listen endpoint**에 적용된다.

```cpp
options.http ()
  .listen ("https://0.0.0.0:8443")
  .configure_tls ([] (zlink::framework::http_tls_options_builder_t &tls) {
      tls.certificate_file ("/etc/pki/game-api.crt.pem")
         .private_key_file ("/etc/pki/game-api.key.pem");
  });
```

listen 없이 `configure_tls`를 부르면 구성 오류로 거부된다.

## 6. server 옵션

운영 한도는 `configure_server`로 조정한다.

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

| 옵션 | 의미 | 기본값 |
|------|------|--------|
| `set_max_connections(n)` | 동시 연결 한도 — 초과 연결은 runtime overload 정책으로 거절한다 | 1024 |
| `set_max_request_body_size(bytes)` / `set_max_header_size(bytes)` | 요청 크기 한도 — 초과 시 413/431 | 1MB / 64KB |
| `set_request_headers_timeout(ms)` / `set_request_body_timeout(ms)` | 수신 단계별 timeout | 5000ms / 5000ms |
| `set_write_timeout(ms)` | 응답 쓰기 timeout | 5000ms |
| `set_keep_alive_timeout(ms)` / `set_max_keep_alive_requests(n)` | keep-alive 연결 유지 정책 | 5000ms / 100 |
| `set_graceful_shutdown_timeout(ms)` | 종료 시 진행 중인 request가 끝나기를 기다리는 시간 | 5000ms |

종료 시 서버는 새 연결을 먼저 막고 진행 중인 request가 끝나기를 기다린다.
timeout 안에 끝나지 않은 연결이나 keep-alive로 대기 중인 연결은 정리하므로
`stop()`이 매달리지 않는다.

## 7. 관련 문서

- 정식 계약: [C++ HTTP hosting 공개 계약](../../../common/spec/server/languages/cpp/60-http-hosting.ko.md)
- handler 모델: [13. 주요 타입 사용 색인](13-interface-catalog.ko.md)
- health endpoint: [11. Monitoring](11-monitoring.ko.md)
