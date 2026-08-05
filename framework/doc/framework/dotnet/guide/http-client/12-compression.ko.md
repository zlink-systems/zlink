[← 목차](README.ko.md)

# 12. 압축

`Compression()`을 켜면 요청에 `Accept-Encoding: gzip, deflate`를 붙이고 응답이
`gzip` 또는 `deflate`로 인코딩됐으면 투명하게 해제한다.

```csharp
var report = await ZLinkHttpClient.Create("https://api.internal")
    .Compression()
    .Get("/large-report")
    .Fetch<Report>();
```

## 래퍼 통제 해제

.NET 네이티브 `AutomaticDecompression`은 끄고 **래퍼가 해제를 통제**한다. 이유는
의미론을 zlink 계약에 맞추기 위해서다:

- **gzip + deflate 모두** 처리한다(deflate는 zlib-wrap/raw 둘 다 감지).
- 해제 후 `Content-Encoding` 헤더를 **제거**한다.
- **decoded 크기**를 `MaxResponseBodySize`로 강제한다.
- `DownloadAsync(sink)` streaming chunk는 **해제하지 않는다**(받은 그대로 전달).

본문이 손상됐으면 `ProtocolError`, decoded 크기가 한도를 넘으면 `CapacityExceeded`로
보고된다.

[다음: 에러 처리 →](13-error-handling.ko.md)
