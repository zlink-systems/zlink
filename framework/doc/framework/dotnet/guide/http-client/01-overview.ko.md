[← 목차](README.ko.md)

# 1. 개요

## 무엇인가

`Zlink.HttpClient`는 .NET 애플리케이션이 HTTP API를 호출할 때 쓰는 client package다.
Client 설정과 request 설정을 fluent builder로 구성하며, redirect·cookie·압축·재시도
정책을 호출부마다 다시 작성하지 않아도 된다.

```csharp
// Client 설정과 request를 한 흐름에서 구성한다.
var profile = await client.Get("/players/7281").Fetch<PlayerProfile>();
```

JSON 전용 client가 아니다. 일반 HTTP client이며 typed 경로
(`Body(dto)` / `Fetch<T>()` / `Async<T>()`)는 그 위에 얹은 편의 계층이다.

## 사용 원칙

- Client 구성과 request 구성은 메서드 체인으로 작성한다.
- Client는 한 번 만들고 재사용한다. 단발 요청에만 one-shot builder를 사용한다.
- Typed body와 response는 기본 JSON codec을 사용한다. 다른 codec이 필요하면 client를
  만들 때 `.NET` codec extension을 등록한다.

## 실행 모델

요청 실행은 .NET의 비동기 기본형 위에서 동작한다.

- `AsyncRaw()` / `Async<T>()` / `DownloadAsync(sink)`는 `ValueTask<T>`를
  돌려준다.
- 완료 값을 동기로 꺼내는 blocking terminator는 제공하지 않는다.
- standalone client는 `Async`와 callback을 제공한다. DI로 주입받는 server client는
  정상 완료 값이 없는 one-way `Async()`도 제공한다. HTTP request builder에는
  Spot 실행 권한을 반납하는 `Yield`가 없다.

이 모델의 실용적 결론 하나만 기억하면 된다:

> 일반 요청은 `Async<T>()`로 기다린다. Spot의 다른 처리를 진행해야 하면 HTTP 요청을
> `RunIoWorker(...)` 안에서 실행하고 worker call의 `Yield()`로 기다린다.

자세한 규칙은 [7. 비동기](07-async.ko.md)에서 다룬다.

## 기능 한눈에 보기

- 메서드: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- body: typed JSON DTO · raw(임의 content-type) · form-urlencoded ·
  multipart/form-data · chunked streaming 업로드
- 응답: raw · typed JSON · streaming 다운로드
- connection pool(네이티브), redirect 추적, transport retry, cookie jar
- 인증: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS 검증, test certificate trust
- HTTP proxy
- gzip/deflate 응답 투명 해제

범위 밖: caller cancellation 공통 모델(표준 `CancellationToken`은 지원).

[다음: 시작하기 →](02-getting-started.ko.md)
