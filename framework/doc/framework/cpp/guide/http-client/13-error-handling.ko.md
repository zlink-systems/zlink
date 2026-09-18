[← 목차](README.ko.md)

# 13. 에러 처리

모든 실패는 `zlink::framework` 공통 에러 모델로 보고된다. 받는 형태는 소비
방법에 따라 둘 중 하나다.

- `result_t` — `.result()` 또는 콜백 submit. `operator bool`로 분기,
  `error()`로 상세 접근.
- `framework_exception_t` 예외 — `co_await`와 `fetch<T>()`. `kind()`와
  `what()`을 가진다.

같은 실패가 두 형태로 표현될 뿐, 분류는 동일하다.

## error kind 매핑

| kind | 언제 | 자동 retry |
|------|------|-----------|
| `protocol_error` | 잘못된 설정/입력: bad base_url·scheme, 0 이하 timeout, 0 bytes 응답 body 상한, 빈 헤더 이름, path가 `/`로 시작 안 함, 복수 body 소스, coroutine execute scheduler 미구성, OpenSSL 없는 빌드의 https, 응답 JSON 디코딩 실패, 손상된 gzip/deflate body | ✗ |
| `unavailable` | transport 실패(연결 거부·끊김, TLS 검증 실패), 초기화되지 않은 client(`client_t{}` 기본 생성 후 사용) | ✓ |
| `deadline_exceeded` | client/request timeout 초과. coroutine client에서는 scheduler queue 등록 시점부터 timeout을 계산한다 | ✓ |
| `rejected` | 응답 body 상한 초과, 압축 해제 크기 상한 초과 | ✗ |
| `internal_failure` | typed 경로의 4xx/5xx, redirect 한도 초과, 지원하지 않는 redirect location, proxy CONNECT 거부 | ✗ |

설정 오류(`protocol_error`)는 의도적으로 transport 실패와 구분된다 —
코드 버그라서 재시도가 무의미하기 때문이다. setter/`build()` 시점에 바로 throw
되는 경우도 많다.

## result_t 패턴

```cpp
auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();

if (!result) {
    const auto *error = result.error ();
    switch (error->kind ()) {
        case zlink::framework::framework_error_kind_t::deadline_exceeded:
            metrics.count ("player_lookup.timeout");
            break;
        case zlink::framework::framework_error_kind_t::protocol_error:
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
    using kind_t = zlink::framework::framework_error_kind_t;
    if (error.kind () == kind_t::unavailable || error.kind () == kind_t::deadline_exceeded) {
        schedule_retry ();
    } else {
        report_permanent_failure (error.what ());
    }
}
```

## 4xx/5xx는 어느 쪽인가

- `submit<T>()`/`fetch<T>()` (typed): **실패** — `internal_failure`,
  "HTTP request failed with status 404".
- `submit_raw()`: **성공** — status를 직접 분기한다
  ([6. Response 다루기](06-handling-responses.ko.md)).

업무 로직이 404/409 같은 status에 의미를 두면 raw 경로를, "200 + DTO 아니면
실패"가 맞으면 typed 경로를 사용한다.

## 자동 retry가 다루는 kind

`retry(attempts)`([10장](10-redirects-retries-cookies.ko.md))는 `unavailable`과
`deadline_exceeded`를 자동으로 다시 시도한다. 직접 재시도 루프를 짤 때도 같은
기준을 사용하면 일관된다. `framework_exception_t`와 `result_t`에는 재시도 여부를 알리는
플래그가 없으므로 kind로 판단한다.

```cpp
using kind_t = zlink::framework::framework_error_kind_t;

for (int attempt = 0;; ++attempt) {
    auto result = client.get ("/ready").submit_raw ().result ();
    const bool retriable = !result
                           && (result.error_kind () == kind_t::unavailable
                               || result.error_kind () == kind_t::deadline_exceeded);
    if (result || attempt >= 3 || !retriable) {
        return result;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (200 << attempt));
}
```

[← 목차](README.ko.md)
