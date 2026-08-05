[← 목차](README.ko.md)

# 13. 에러 처리

실패는 `ZLinkFrameworkException`(`Zlink.Framework.Contracts.Errors`)으로 보고된다.
`Kind`(`ZLinkFrameworkErrorKind`)는 실패 종류를 나타낸다. 예외는 retry hint를 제공하지 않으므로,
application은 operation의 idempotency와 side effect를 확인해 다음 동작을 결정한다.

## error kind 매핑

| 상황 | `Kind` |
|------|--------|
| 요청 형식, redirect 또는 응답 decode 오류 | `ProtocolError` |
| DNS, proxy CONNECT 또는 target 연결 실패 | `Unavailable` |
| 응답 본문 또는 압축 해제 크기 제한 초과 | `CapacityExceeded` |
| 요청 timeout | `DeadlineExceeded` |
| HTTP status 400 이상 | `InternalFailure` |

## timeout

timeout은 `DeadlineExceeded`다. HTTP client의 `Retry(attempts)`는 request를 다시 만들 수 있는
transport failure와 timeout에 내부적으로 적용되지만, application exception에는 retry hint를 싣지 않는다.

## 재시도 판단

재시도 가능한 transport 오류와 timeout만 `Retry(attempts)` 설정에 따라 다시 시도한다. HTTP status 오류와
protocol 오류는 다시 시도하지 않는다.
Streaming 요청도 retry에서 제외된다([10장](10-redirects-retries-cookies.ko.md)).

## 예외 경로 정리

```csharp
try
{
    var res = await client.Post("/games").Body(req).Fetch<CreateGameRes>();
}
catch (ZLinkFrameworkException ex)
    when (ex.Kind == ZLinkFrameworkErrorKind.DeadlineExceeded)
{
    // timeout이다. 재시도 여부는 request의 idempotency와 side effect를 확인해 결정한다.
}
catch (ZLinkFrameworkException ex)
    when (ex.Kind == ZLinkFrameworkErrorKind.Unavailable)
{
    // 현재 target에 연결할 수 없다.
}
catch (ZLinkFrameworkException ex)
    when (ex.Kind == ZLinkFrameworkErrorKind.ProtocolError)
{
    // 요청 형식, redirect 또는 응답 decode 오류다.
}
```

[← 목차](README.ko.md)
