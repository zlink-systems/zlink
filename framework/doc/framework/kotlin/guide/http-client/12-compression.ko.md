[← 목차](README.ko.md)

# 12. 압축

`compression()`을 켜면 요청에 `Accept-Encoding: gzip, deflate`를 붙이고 응답이
`gzip` 또는 `deflate`로 인코딩됐으면 투명하게 해제한다.

```kotlin
val report = zlinkHttpClient("https://api.internal") {
    compression()
}.use { client ->
    client.get("/large-report").fetch<Report>()
}
```

## 해제 규칙

- **gzip + deflate 모두** 처리한다(deflate는 zlib-wrap/raw 둘 다 감지).
- 해제 후 `content-encoding` 헤더를 **제거**한다.
- **decoded 크기**를 `maxResponseBodySize`로 강제한다.
- `awaitDownload(sink)` streaming chunk는 **해제하지 않는다**(받은 그대로 전달).

본문이 손상됐으면 decode 실패 예외, decoded 크기가 한도를 넘으면 request 실패 예외로
보고된다.

[다음: 에러 처리 →](13-error-handling.ko.md)
