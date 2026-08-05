[← 목차](README.ko.md)

# 8. Streaming

## streaming 다운로드

`download(sink)`는 응답 본문을 버퍼링하지 않고 chunk 단위로 sink에 전달한다. 반환되는
응답은 status와 헤더를 담고 본문은 비어 있다. chunk는 받은 그대로 전달되며
**content-encoding 해제를 적용하지 않는다**(압축 옵션과 무관).

```java
try (var file = Files.newOutputStream(Path.of("report.bin"))) {
    RawHttpResponse response = client.get("/reports/large")
        .download(chunk -> file.write(chunk))
        .toCompletableFuture().join();
}
```

- sink는 `Consumer<byte[]>` 타입이며 본문을 읽는 executor 스레드에서 호출된다.
- 누적 크기는 `maxResponseBodySize`로 제한된다.

## streaming 업로드

`bodyStream(provider, contentType)`는 요청 본문을 chunk 단위로 chunked transfer-encoding으로
전송한다. provider(`Supplier<byte[]>`)가 `null`을 돌려주면 본문이 끝난다.

```java
client.post("/upload-stream")
    .bodyStream(() -> nextChunk(), "application/octet-stream")
    .submitRaw();
```

streaming 업로드는 provider를 rewind할 수 없으므로 **자동 retry에서 제외**된다
([10장](10-redirects-retries-cookies.ko.md)).

[다음: 인증과 TLS →](09-authentication-tls.ko.md)
