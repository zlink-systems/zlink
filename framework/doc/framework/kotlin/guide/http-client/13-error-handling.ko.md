[← 목차](README.ko.md)

# 13. 에러 처리

실패는 `ZLinkFrameworkException`(`systems.zlink.framework.errors`)으로 보고된다.

## 예외 모델

`ZLinkFrameworkException`은 `RuntimeException` 기반이며 `kind()`로 `ZLinkFrameworkErrorKind`를
노출한다. 재시도 여부를 알리는 `isRetriable` 같은 플래그는 없다. http-client는
`PROTOCOL_ERROR`와 `INTERNAL_FAILURE`를 사용한다. suspend 확장은 `CompletionException` 포장을
제거하고 원인 예외를 그대로 던진다.

| 상황 | `kind()` |
|------|------|
| 구성/요청 검증 실패(base url, path, single body source, proxy scheme, 0 timeout 등) | `PROTOCOL_ERROR` |
| status ≥ 400 (`await`/`fetch`) | `INTERNAL_FAILURE` |
| redirect 한도 초과 | `INTERNAL_FAILURE` |
| 응답 JSON 디코드 실패 | `PROTOCOL_ERROR`(원인 cause 포함) |
| 압축 본문 손상 | `PROTOCOL_ERROR` |
| 압축 decoded 크기 초과 / 본문 크기 초과 | `INTERNAL_FAILURE` |
| transport 실패(연결 오류, timeout) | `INTERNAL_FAILURE`(원인은 `IOException` 또는 `TimeoutException`) |

## 재시도 판단

retry 판단은 내부적으로 원인 예외가 **`IOException`·`UncheckedIOException`·`TimeoutException`**
인지로 한다. status 코드 실패(4xx/5xx)는 재시도하지 않는다. streaming 요청은 retry에서 제외된다
([10장](10-redirects-retries-cookies.ko.md)). `retry`가 설정돼 있으면 해당 실패가 재시도된다.
공개 표면에는 재시도 판단이 드러나지 않으므로, application은 operation의 idempotency를 확인해
다음 동작을 결정한다.

## 예외 경로

suspend 호출은 `try`/`catch`로 직접 잡는다.

```kotlin
try {
    val res = client.post("/games").body(req).await<CreateGameRes>()
    // 성공
} catch (e: ZLinkFrameworkException) {
    // 4xx/5xx, transport, decode 실패 등
}
```

[← 목차](README.ko.md)
