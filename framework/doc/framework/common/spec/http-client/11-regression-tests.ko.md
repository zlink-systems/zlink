# 11. 회귀 테스트 계약

> [공통 계약 목차](README.ko.md)

각 언어는 아래 **공통 계약 케이스 매트릭스**를 실서버 기반(in-process HTTP/TLS
테스트 서버) 계약 테스트로 검증한다. 케이스 명세는 이 문서가 정본이고,
언어별 테스트 파일이 구현이다.

## 11.1 테스트 위치

| 언어 | 계약 테스트 | 방식 |
| --- | --- | --- |
| cpp | `http-client/tests/test_cpp_http_client.cpp` | gtest + in-process Beast 서버(TLS 포함) |
| dotnet | `tests/Zlink.HttpClient.UnitTests/HttpClientContractTests.cs` (+`RuntimeUnitTests.cs`) | xunit + `HttpListener` 서버 |
| java | `zlink-http-client/src/test/.../HttpClientContractTest.java` (+`CookieJarTest`) | JUnit + `com.sun.net.httpserver` |
| kotlin | `zlink-http-client-kotlin/src/test/.../HttpClientCoroutineTest.kt` | suspend 브리지·DSL·비직렬화만(전송 의미론은 java가 책임) |
| node | `test/contract/http-client.test.js` | `node:test` + `node:http/https/net` 서버 |

## 11.2 공통 케이스 매트릭스

새 언어/새 기능은 이 매트릭스를 기준으로 누락을 판정한다.

**요청 조립**: 7개 verb 디스패치 · path `/` 검증 · query percent-encoding ·
default 헤더 + 요청별 override · Basic/Bearer 주입.

**body**: typed JSON 왕복 · raw · form · multipart · 바디 소스 배타 위반 →
`ProtocolError` · streaming 업로드가 실제 chunked인지(raw socket 검증).

**응답**: HEAD 빈 body · 204/빈 성공의 typed null body · typed status ≥ 400 →
`InternalFailure` · malformed JSON → `ProtocolError` ·
`maxResponseBodySize` 강제.

**redirect**: 303 POST→GET rewrite · same-origin `Authorization` 보존 ·
cross-origin 제거 · 한도 초과 · 상대/절대 Location.

**retry/timeout**: 전송 실패 후 재시도 성공 · 재시도 소진 · timeout →
자동 retry 대상 · streaming 재시도 제외.

**cookie**: 저장/전송 왕복 · Path scope 매칭 · Secure의 http 미전송 ·
`Max-Age<=0` 삭제 · host당 128개 축출(현재 cpp 미검증 — 갭).

**압축**: gzip/deflate 투명 해제 · 손상 body → `ProtocolError` ·
해제 후 크기 한도.

**TLS/proxy**: trust 인증서로 성공 · untrusted 거부 · hostname mismatch 거부 ·
mTLS 제시 · proxy 평문/CONNECT/인증(언어별 가능 범위).

**실행 모델**: 20개 동시 요청이 직렬화되지 않음(non-blocking 증명) ·
one-shot 경로 동작 · (cpp) 커스텀 execute/resume scheduler.

## 11.3 게이트

- 커버리지: java/kotlin JaCoCo LINE ≥ 0.80, node 내장 coverage 게이트 80%.
  dotnet/cpp는 계약 테스트 전량 그린이 게이트.
- 교차 언어 검증: node `verify:cross-language` 게이트가 존재한다. 공통 스펙
  확정 후 5개 언어 매트릭스 대조 게이트로 확장한다(plan 추적).

## 11.4 알려진 커버리지 갭 (plan에서 추적)

- cpp: pool eviction/TTL, cookie 128 축출, IPv6 URL, 기본 스케줄러
  직렬화/데드락, gzip 헤더 파서 fuzz.
- dotnet: 명시적 caller-cancellation, 307/308 body 보존 재시도.
- 공통: 멀티스레드 동시성 스트레스(cookie jar/pool).
