[← 목차](README.ko.md)

# 8. Streaming

응답이나 요청 body가 메모리에 다 올리기엔 클 때 streaming 경로를 쓴다.

## 다운로드: download(sink)

`download(sink)`는 응답 body를 버퍼링하지 않고 도착하는 대로 chunk 단위로
sink에 전달한다. 반환되는 응답에는 status와 헤더만 있고 body는 비어 있다.
다만 전체 body bytes는 client의 `max_response_body_size` 상한을 넘을 수 없다. 큰 파일을
다운로드하는 client는 이 값을 명시적으로 올려야 한다.

```cpp
// 수 GB짜리 리플레이 파일을 디스크로 흘려 쓴다
std::ofstream out ("/var/games/replays/r-99182.bin", std::ios::binary);

auto result = client.get ("/replays/r-99182.bin")
                .timeout (std::chrono::minutes (5))
                .download ([&out] (std::string_view chunk) {
                    out.write (chunk.data (),
                               static_cast<std::streamsize> (chunk.size ()));
                })
                .result ();

if (!result) {
    std::filesystem::remove ("/var/games/replays/r-99182.bin");   // 부분 파일 정리
    throw std::runtime_error (result.error ()->what ());
}
assert (result.value ().status == 200);
assert (result.value ().body.empty ());   // body는 sink로 갔다
```

진행률이 필요하면 `Content-Length` 헤더와 누적 바이트로 계산한다.

```cpp
std::uint64_t received = 0;
auto result = client.get ("/replays/r-99182.bin")
                .download ([&] (std::string_view chunk) {
                    received += chunk.size ();
                    progress_bar.update (received);
                    out.write (chunk.data (), chunk.size ());
                })
                .result ();
```

### redirect와의 상호작용

`follow_redirects()`가 켜진 client에서 download하면 **중간 redirect 응답의
body는 sink로 새지 않는다**. 최종 응답 body만 전달된다.

## 업로드: body_stream

대용량 요청 body는 [5장의 body_stream](05-request-body.ko.md)으로 chunked
전송한다. provider가 chunk를 공급하다 `std::nullopt`로 끝낸다.

## streaming 경로의 제약

되감을 수 없는 데이터 흐름이므로 일반 경로와 달리 다음이 적용되지 않는다.

| 기능 | streaming에서 |
|------|---------------|
| 자동 retry (`retry(n)`) | **제외** — sink가 중복 chunk를 받거나 provider가 재생 불가하므로 |
| connection pool 재사용 | `body_stream`은 항상 fresh 연결 (download는 재사용함) |
| 압축 해제 (`compression()`) | download sink에는 **원시 bytes 그대로** 전달 (해제 안 함) |

retry가 필요하면 호출자가 실패 시 sink/provider를 새로 준비해 다시 호출한다.

```cpp
for (int attempt = 0; attempt < 3; ++attempt) {
    std::ofstream out (path, std::ios::binary | std::ios::trunc);   // 매 시도 새로
    auto result = client.get ("/replays/r-99182.bin")
                    .download ([&out] (std::string_view c) { out.write (c.data (), c.size ()); })
                    .result ();
    if (result || !result.error ()->is_retriable ()) {
        break;
    }
}
```

[다음: 인증과 TLS →](09-authentication-tls.ko.md)
