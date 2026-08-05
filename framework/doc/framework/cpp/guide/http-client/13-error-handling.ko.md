[← 목차](README.ko.md)

# 13. 에러 처리

모든 실패는 `zlink::framework` 공통 에러 모델로 보고된다. 받는 형태는 소비
방법에 따라 둘 중 하나다.

- `result_t` — `.result()` 또는 콜백 submit. `operator bool`로 분기,
  `error()`로 상세 접근.
- `framework_exception_t` 예외 — `co_await`와 `fetch<T>()`. `kind()`,
  `what()`, `is_retriable()`을 가진다.

같은 실패가 두 형태로 표현될 뿐, 분류는 동일하다.

## error kind 매핑

| kind | 언제 | retriable |
|------|------|-----------|
| `request_protocol_error` | 잘못된 설정/입력: bad base_url·scheme, 0 이하 timeout, 0 bytes 응답 body 상한, 빈 헤더 이름, path가 `/`로 시작 안 함, 복수 body 소스, `nullptr` coroutine scheduler, OpenSSL 없는 빌드의 https | ✗ |
| `request_failed` | transport 실패(연결 거부·끊김, TLS 검증 실패), typed 경로의 4xx/5xx, redirect 한도 초과, 응답 body 상한 초과, proxy CONNECT 거부 | transport는 ✓, 나머지 ✗ |
| `timeout` | client/request timeout 초과. coroutine client에서는 scheduler queue 등록 시점부터 timeout을 계산한다 | ✓ |
| `payload_decode_failed` | 응답 JSON 디코딩 실패, 손상된 gzip/deflate body | ✗ |
| `closed` | 초기화되지 않은 client (`client_t{}` 기본 생성 후 사용), custom execute scheduler가 작업 등록을 거부함 | ✗ |

설정 오류(`request_protocol_error`)는 의도적으로 transport 실패와 구분된다 —
코드 버그라서 재시도가 무의미하기 때문이다. setter/`build()` 시점에 바로 throw
되는 경우도 많다.

## result_t 패턴

```cpp
auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();

if (!result) {
    const auto *error = result.error ();
    switch (error->kind ()) {
        case zlink::framework::framework_error_kind_t::timeout:
            metrics.count ("player_lookup.timeout");
            break;
        case zlink::framework::framework_error_kind_t::payload_decode_failed:
            log_error ("schema mismatch: {}", error->what ());
            break;
        default:
            log_error ("player lookup failed: {}", error->what ());
    }
    return std::nullopt;
}
return result.value ().body;
```

## 예외 패턴 (co_await / fetch)

```cpp
try {
    auto profile = client.get ("/players/7281").fetch<player_profile_t> ();
    render (profile);
}
catch (const zlink::framework::framework_exception_t &error) {
    if (error.is_retriable ()) {
        schedule_retry ();
    } else {
        report_permanent_failure (error.what ());
    }
}
```

## 4xx/5xx는 어느 쪽인가

- `submit<T>()`/`fetch<T>()` (typed): **실패** — `request_failed`,
  "HTTP request failed with status 404".
- `submit_raw()`: **성공** — status를 직접 분기한다
  ([6. Response 다루기](06-handling-responses.ko.md)).

업무 로직이 404/409 같은 status에 의미를 두면 raw 경로를, "200 + DTO 아니면
실패"가 맞으면 typed 경로를 쓴다.

## is_retriable과 자동 retry의 관계

`retry(attempts)`([10장](10-redirects-retries-cookies.ko.md))가 자동 재시도하는
범위가 바로 `is_retriable() == true`인 실패다. 직접 재시도 루프를 짤 때도 같은
기준을 쓰면 일관된다.

```cpp
for (int attempt = 0;; ++attempt) {
    auto result = client.get ("/ready").submit_raw ().result ();
    if (result || attempt >= 3 || !result.error ()->is_retriable ()) {
        return result;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (200 << attempt));
}
```

[← 목차](README.ko.md)
