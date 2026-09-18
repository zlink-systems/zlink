[← 목차](README.ko.md)

# 13. 에러 처리

실패는 `ZLinkFrameworkException`(`@zlink-systems/framework`)으로 보고된다. `kind`
(`ZLinkFrameworkErrorKind`)로 실패 종류를 구분한다. 예외 자체는 재시도 hint를 제공하지 않는다.

## error kind 매핑

| 상황 | kind |
|------|------|
| 구성/요청 검증 실패(base_url, path, single body source, proxy scheme, 0 timeout 등) | `ProtocolError` |
| status ≥ 400 (`async<T>`) | `InternalFailure` |
| redirect 한도 초과 | `Unavailable` |
| 응답 JSON 디코드 실패 | `ProtocolError` |
| 압축 본문 손상 | `ProtocolError` |
| 압축 decoded 크기 초과 / 본문 크기 초과 | `Unavailable` |
| transport 실패(연결 오류 등) | `Unavailable` |
| 요청 timeout | `DeadlineExceeded` |

## timeout

timeout은 `DeadlineExceeded`로 보고된다. `retry`가 설정돼 있으면 재시도된다.

## 재시도 판단

`retry(attempts)`는 `Unavailable`과 `DeadlineExceeded`를 자동으로 다시 시도한다. status 코드 실패(4xx/5xx)와 `ProtocolError`는 재시도하지 않는다. streaming 요청은
retry에서 제외된다([10장](10-redirects-retries-cookies.ko.md)). 예외에는 `isRetriable` 같은
재시도 플래그가 없으므로, application은 operation의 idempotency를 확인해 다음 동작을 결정한다.

## 예외 경로 정리

```ts
try {
  const res = await client.post('/games').body(req).async<CreateGameRes>();
} catch (error) {
  if (error instanceof ZLinkFrameworkException) {
    switch (error.kind) {
      case ZLinkFrameworkErrorKind.InternalFailure: /* HTTP status 400 이상 */ break;
      case ZLinkFrameworkErrorKind.Unavailable: /* transport 실패 */ break;
      case ZLinkFrameworkErrorKind.DeadlineExceeded: /* 요청 timeout */ break;
      case ZLinkFrameworkErrorKind.ProtocolError: /* 요청 검증 또는 응답 본문 디코드 실패 */ break;
      default: break;
    }
  }
}
```

[← 목차](README.ko.md)
