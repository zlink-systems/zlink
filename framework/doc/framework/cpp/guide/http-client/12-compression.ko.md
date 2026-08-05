[← 목차](README.ko.md)

# 12. 압축

`compression()`을 켜면 요청에 `Accept-Encoding: gzip, deflate`를 싣고 서버가
압축해 보낸 응답 body를 **투명하게 해제**한다. 호출 코드는 압축 여부를 모른다.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .compression ()
                .build ();

// 서버가 gzip으로 보내도 body는 평문 JSON으로 디코딩된다
auto board = client.get ("/leaderboard")
               .query ("season", "2026q2")
               .fetch<leaderboard_t> ();
```

해제 후에는 `Content-Encoding` 헤더가 응답에서 제거되므로, 호출자가 보는 응답은
처음부터 평문이었던 것과 동일하다.

## 지원 인코딩

| Content-Encoding | 처리 |
|------------------|------|
| `gzip` | 해제 (gzip 헤더 파싱 + DEFLATE) |
| `deflate` | 해제 (zlib wrapper 자동 감지, raw DEFLATE fallback) |
| 그 외 (`br` 등) | 해제하지 않고 원문 그대로 반환 |

구현은 Boost.Beast 내장 zlib을 사용하므로 외부 zlib 의존성이 없다.

## 제약

- **trailer checksum(CRC32) 미검증** — gzip/zlib trailer의 무결성 검사는 하지
  않는다. 전송 무결성은 TCP/TLS가 보장한다는 전제다.
- **`download(sink)`에는 적용되지 않음** — streaming 다운로드는 원시 bytes를
  그대로 전달한다 ([8. Streaming](08-streaming.ko.md)). 압축된 대용량 파일은
  받아서 호출자가 해제한다.
- 손상된 압축 body는 `payload_decode_failed`로 닫힌다
  ([13. 에러 처리](13-error-handling.ko.md)).

[다음: 에러 처리 →](13-error-handling.ko.md)
