[← 목차](README.ko.md)

# 13. 에러 처리

실패는 `ZLinkFrameworkException`(`@zlink-systems/framework`)으로 보고된다. `kind`
(`ZLinkFrameworkErrorKind`)와 `isRetriable`을 노출한다.

## error kind 매핑

| 상황 | kind |
|------|------|
| 구성/요청 검증 실패(base_url, path, single body source, proxy scheme, 0 timeout 등) | `requestProtocolError` |
| status ≥ 400 (`submit<T>`) | `requestFailed` |
| redirect 한도 초과 | `requestFailed` |
| 응답 JSON 디코드 실패 | `payloadDecodeFailed` |
| 압축 본문 손상 | `payloadDecodeFailed` |
| 압축 decoded 크기 초과 / 본문 크기 초과 | `requestFailed` |
| transport 실패(연결 오류 등) | `requestFailed` (`isRetriable = true`) |

## timeout

Node framework의 `ZLinkFrameworkErrorKind`에는 timeout 전용 kind가 없다. 따라서 Node에서는
timeout을 **`ZLinkFrameworkException(requestFailed, isRetriable: true)`**로 보고한다.
`retry`가 설정돼 있으면 재시도된다.

## retriable

`isRetriable`이 `true`인 실패(transport 오류, timeout)는 `retry(attempts)`가 설정돼
있을 때 재시도된다. status 코드 실패(4xx/5xx)는 retriable이 아니다. streaming 요청은
retry에서 제외된다([10장](10-redirects-retries-cookies.ko.md)).

## 예외 경로 정리

```ts
try {
  const res = await client.post('/games').body(req).submit<CreateGameRes>();
} catch (error) {
  if (error instanceof ZLinkFrameworkException) {
    switch (error.kind) {
      case ZLinkFrameworkErrorKind.RequestFailed: /* 4xx/5xx 또는 transport/timeout */ break;
      case ZLinkFrameworkErrorKind.PayloadDecodeFailed: /* 응답 본문 디코드 실패 */ break;
      default: break;
    }
  }
}
```

[← 목차](README.ko.md)
