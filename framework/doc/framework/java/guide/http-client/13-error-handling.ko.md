[← 목차](README.ko.md)

# 13. 에러 처리

실패는 `ZLinkFrameworkException`(`systems.zlink.framework.errors`)으로 보고된다.

## 예외 모델

**Java framework의 `ZLinkFrameworkException`은 kind enum이나 `isRetriable`을 노출하지
않는다**(`RuntimeException` 기반, 메시지·예외 타입으로 구분하는 Java 관용). http-client도
Java 관용을 따른다.

| 상황 | 보고 |
|------|------|
| 구성/요청 검증 실패(base_url, path, single body source, proxy scheme, 0 timeout 등) | `ZLinkFrameworkException` |
| status ≥ 400 (`submit`/`fetch`) | `ZLinkFrameworkException` |
| redirect 한도 초과 | `ZLinkFrameworkException` |
| 응답 JSON 디코드 실패 | `ZLinkFrameworkException`(원인 cause 포함) |
| 압축 본문 손상 | `ZLinkFrameworkException` |
| 압축 decoded 크기 초과 / 본문 크기 초과 | `ZLinkFrameworkException` |
| transport 실패(연결 오류, timeout) | `ZLinkFrameworkException`(원인은 `IOException`) |

## retriable

retry 판단은 내부적으로 **`IOException`(전송 오류·`HttpTimeoutException`)** 여부로 한다.
status 코드 실패(4xx/5xx)는 재시도하지 않는다. streaming 요청은 retry에서 제외된다
([10장](10-redirects-retries-cookies.ko.md)). `retry`가 설정돼 있으면 retriable 실패가
async 합성으로 재시도된다.

## 예외 경로 정리

```java
client.post("/games").body(req).submit(CreateGameRes.class)
    .exceptionally(error -> {
        Throwable cause = error.getCause();   // CompletionException 언래핑
        if (cause instanceof ZLinkFrameworkException) {
            // 4xx/5xx, transport, decode 실패 등
        }
        throw new CompletionException(error);
    });
```

테스트·CLI에서는 `fetch(Type)`가 실패를 동기 예외로 던진다.

[← 목차](README.ko.md)
