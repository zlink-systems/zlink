[← 목차](README.ko.md)

# 6. Response 다루기

## raw 응답

`AsyncRaw()`는 `RawHttpResponse`를 돌려준다.

```csharp
RawHttpResponse response = await client.Get("/players/7281").AsyncRaw();
int status = response.Status;
string body = response.Body;
string contentType = response.Headers["content-type"];
```

`Headers`는 대소문자 무시 조회를 지원한다.

## typed JSON 응답

`Async<T>()`는 응답을 client의 codec으로 디코드해 `HttpResponse<T>`를 돌려준다.
별도 codec extension을 등록하지 않으면 JSON을 사용한다.

```csharp
HttpResponse<PlayerProfile> response = await client.Get("/players/7281").Async<PlayerProfile>();
PlayerProfile profile = response.Body;     // 디코드된 DTO
string raw = response.RawBody;             // 원본 응답 텍스트
```

- status가 **400 이상**이면 `ZLinkFrameworkException(InternalFailure)`를 던진다.
- 본문 디코드 실패는 `ZLinkFrameworkException(ProtocolError)`로 보고된다.

## 본문만 필요할 때

`Fetch<T>()`는 같은 디코드를 수행하고 **디코드된 본문만** 돌려준다. status와 header를
보지 않는 호출은 이쪽이 짧다.

```csharp
PlayerProfile profile = await client.Get("/players/7281").Fetch<PlayerProfile>();
```

status나 header를 함께 봐야 할 때만 `Async<T>()`로 `HttpResponse<T>`를 받는다.

## status 처리 정리

| 경로 | 4xx/5xx |
|------|---------|
| `AsyncRaw()` | status를 그대로 돌려준다(예외 없음) |
| `Fetch<T>()` / `Async<T>()` / typed callback | `InternalFailure` 오류 |

[다음: 비동기 →](07-async.ko.md)
