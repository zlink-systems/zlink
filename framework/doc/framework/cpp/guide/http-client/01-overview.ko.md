[← 목차](README.ko.md)

# 1. 개요

## 무엇인가

`zlink::http_client`는 C++ 애플리케이션이 HTTP API를 호출할 때 쓰는 client-side
산출물이다. C++ 표준 라이브러리에는 HTTP client가 없고 Boost.Beast를 직접 쓰면
socket·resolver·parser 같은 낮은 수준 타입이 application 코드에 흘러들어온다.
이 client는 그 복잡성을 fluent builder 뒤로 숨긴다.

```cpp
// Boost.Beast 직접 사용: resolver, stream, request<string_body>, flat_buffer ...
// zlink::http_client: 아래 한 문장
auto profile = client.get ("/players/7281").fetch<player_profile_t> ();
```

JSON 전용 client가 아니다. 일반 HTTP client이며 typed JSON 경로
(`body(dto)` / `submit<T>()` / `fetch<T>()`)는 그 위에 얹은 편의 계층이다.

## 설계 원칙

- **fluent builder.** client 구성과 request 구성 모두 메서드 체인으로 쓴다.
- **public header에 Beast 없음.** `Boost.Beast`, `Boost.Asio`, OpenSSL, socket,
  resolver, parser 타입은 public header에 드러나지 않는다. 의존성은 runtime
  구현(private) 안에 갇힌다.
- **request가 client를 소유.** request builder는 client를 값으로 보유하므로,
  임시 client로 만든 단발 request도 use-after-free 없이 안전하다.

## 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| facade header | `http-client/include/zlink/http_client.hpp` | public |
| contract header | `http-client/include/zlink/http_client/contracts/*` | public |
| runtime 구현 | `http-client/src/runtime/*` | private |
| 회귀 테스트 | `http-client/tests/*` | private |
| CMake target | `zlink::http_client` | public target |

## 실행 모델

요청 실행은 client 설정에 따라 두 가지로 나뉜다.

- 기본 client는 기존처럼 `submit_raw()`/`submit<T>()` 호출 중 HTTP 교환을 동기로
  실행한다. 이 동작은 기존 blocking 코드와 테스트를 깨지 않기 위한 기본값이다.
- `.coroutines()`를 명시한 client는 `submit_raw()`/`submit<T>()` 호출 시 HTTP 작업을
  내부 scheduler에 등록하고 `task_t`를 돌려준다. `co_await`는 응답이 준비될 때까지
  호출 스레드를 점유하지 않고 suspend된다.

이 모델의 실용적 결론 하나만 기억하면 된다:

> framework runtime/handler 스레드 안에서는 `submit<T>()`를 `co_await`하고,
> `.result()`/`fetch<T>()` 같은 blocking 접근은 테스트·client 시나리오처럼 blocking이
> 허용되는 곳에서만 쓴다. handler 안에서 HTTP 대기 중 스레드를 비우려면 client를
> `.coroutines()` 또는 server가 제공한 resume scheduler로 구성한다.

자세한 규칙은 [7. 비동기와 코루틴](07-async-coroutines.ko.md)에서 다룬다.

## 기능 한눈에 보기

- 메서드: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- body: typed JSON DTO · raw(임의 content-type) · form-urlencoded ·
  multipart/form-data · chunked streaming 업로드
- 응답: raw · typed JSON · streaming 다운로드
- connection keep-alive pool, redirect 추적, transport retry, cookie jar
- 인증: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS 검증, test certificate trust
- HTTP proxy (absolute-form + `CONNECT` tunnel)
- gzip/deflate 응답 투명 해제

범위 밖: HTTP/2(Boost.Beast 미지원), caller cancellation 공통 모델.

[다음: 시작하기 →](02-getting-started.ko.md)
