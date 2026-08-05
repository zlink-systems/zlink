[← 목차](README.ko.md)

# 3. Client 구성

`client_t::create()`가 돌려주는 `client_builder_t`에서 client 수준 설정을 모두
정한다. `build()`가 호출되는 순간 설정이 확정되고 connection pool을 가진
runtime이 만들어진다.

## 전체 옵션

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .timeout (std::chrono::seconds (5))
                .max_response_body_size (32 * 1024 * 1024)
                .default_header ("x-service-name", "matchmaker")
                .bearer_token (session_token)
                .trust_certificate_file ("/etc/pki/internal-root-ca.pem")
                .follow_redirects (5)
                .retry (2)
                .cookies ()
                .compression ()
                .build ();
```

| 옵션 | 의미 | 기본값 |
|------|------|--------|
| `base_url(url)` / `create(url)` | `http://` 또는 `https://` endpoint. path prefix 포함 가능 | 필수 |
| `timeout(duration)` | request 기본 timeout. request 단위로 [override 가능](04-making-requests.ko.md) | 3000ms |
| `max_response_body_size(bytes)` | 응답 body를 읽을 때 허용하는 최대 bytes. buffered 응답과 streaming 다운로드 모두에 적용 | 16 MiB |
| `default_header(name, value)` | 모든 request에 실리는 헤더 | 없음 |
| `basic_auth(user, pw)` / `bearer_token(tok)` | `Authorization` 헤더 ([9장](09-authentication-tls.ko.md)) | 없음 |
| `trust_certificate_file(path)` | 추가로 신뢰할 server certificate ([9장](09-authentication-tls.ko.md)) | 시스템 CA |
| `client_certificate_file(cert, key)` | mTLS client certificate ([9장](09-authentication-tls.ko.md)) | 없음 |
| `follow_redirects(max = 5)` | redirect 자동 추적 — 무인자 호출 시 한도 5회 ([10장](10-redirects-retries-cookies.ko.md)) | off (호출 시 활성) |
| `retry(attempts)` | retriable transport 실패 재시도 ([10장](10-redirects-retries-cookies.ko.md)) | off |
| `cookies()` | in-memory cookie jar ([10장](10-redirects-retries-cookies.ko.md)) | off |
| `proxy(url)` / `proxy_basic_auth(user, pw)` | HTTP proxy ([11장](11-proxy.ko.md)) | 없음 |
| `compression()` | gzip/deflate 응답 해제 ([12장](12-compression.ko.md)) | off |

잘못된 값(빈 base_url, `ftp://` scheme, 0 이하 timeout, 0 bytes 응답 body 상한,
빈 헤더 이름 등)은
`build()` 또는 해당 setter에서 `request_protocol_error`로 즉시 던진다 —
조용히 넘어가지 않는다.

`default_header`로 넣은 헤더는 redirect 대상이 바뀌어도 그대로 적용된다. 비밀 값은
`default_header`에 직접 넣지 말고 `basic_auth`나 `bearer_token`을 사용한다. 이 두 인증
API가 만드는 `Authorization` 헤더는 교차 origin redirect에서 자동으로 제거된다.

## base_url과 path prefix

`base_url`에 path를 포함하면 모든 request path 앞에 붙는다.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal/v2")
                .build ();
client.get ("/players/7281");   // 실제 target: /v2/players/7281
```

## client 재사용과 connection pool

`client_t`는 내부 runtime을 `shared_ptr`로 공유하는 가벼운 핸들이다. 복사해도
같은 connection pool과 cookie jar를 공유한다.

같은 origin으로 가는 요청은 **keep-alive 연결을 자동 재사용**한다. 서버가 그 사이
연결을 닫았다면(stale) fresh 연결로 1회 자동 재시도하므로 호출자는 신경 쓸 것이
없다.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .build ();

// 세 요청 모두 같은 TCP 연결을 재사용한다
client.get ("/games/active").submit_raw ().result ();
client.get ("/players/7281").submit_raw ().result ();
client.get ("/leaderboard").submit_raw ().result ();
```

권장 패턴:

- **서비스당 client 하나를 만들어 재사용**한다. 요청마다 `create()...build()`를
  반복하면 매번 새 runtime(pool)이 생겨 keep-alive 이득이 사라진다.
- 단발 요청만 하는 곳은 [build() 생략 shortcut](02-getting-started.ko.md)이
  더 간결하다.

## 스레드 안전성

- `client_t`와 그 복사본으로 **여러 스레드에서 동시에 요청해도 안전**하다.
  connection pool과 cookie jar는 내부적으로 동기화된다.
- `client_builder_t`/`request_builder_t`는 구성 중인 객체이므로 스레드 간에
  공유하지 않는다.

[다음: Request 만들기 →](04-making-requests.ko.md)
