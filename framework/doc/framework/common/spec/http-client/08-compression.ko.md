# 8. 압축

> [공통 계약 목차](README.ko.md)

`compression()` opt-in 계약:

- 요청에 `Accept-Encoding: gzip, deflate`를 붙인다(opt-in 전에는 붙이지 않음).
- `Content-Encoding: gzip | deflate` 응답을 **투명 해제**하고, 해제 후
  `content-encoding`(및 관련 length) 헤더를 응답 map에서 제거한다.
  사용자는 압축이 없었던 것처럼 body를 본다.
- deflate는 zlib-wrapped와 raw 두 형태를 모두 수용한다(선두 바이트 감지).
- **해제 후 크기에도 `maxResponseBodySize`를 강제**한다(압축 폭탄 방어).
  초과 시 `CapacityExceeded`.
- 손상된 압축 body는 `ProtocolError`.
- **streaming download에는 해제를 적용하지 않는다** — sink는 원시(압축된)
  바이트를 받는다([4장 §4.4](04-response-model.ko.md)).
- 언어 매핑: cpp Boost.Beast zlib(gzip 헤더 자체 파싱, CRC32 trailer 미검증),
  dotnet `System.IO.Compression`(네이티브 `AutomaticDecompression`은 끔),
  java `java.util.zip`, node `node:zlib`.
