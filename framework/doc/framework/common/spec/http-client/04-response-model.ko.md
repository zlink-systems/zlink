# 4. Response 계약

> [공통 계약 목차](README.ko.md)

## 4.1 terminator 축(§5.1)

| 형태 | 종결자(이름은 [언어별 인터페이스 §1.4](language-interfaces.ko.md#14-종결자-terminator)) | 반환 | 실패 조건 |
| --- | --- | --- | --- |
| raw | raw response terminator | `RawHttpResponse` | 전송 실패만. **status는 실패 아님**(4xx/5xx도 성공 반환) |
| typed | typed response terminator | `HttpResponse<T>` | 전송 실패 + **status ≥ 400** + 디코드 실패 |
| 다운로드 | download terminator | `RawHttpResponse`(body 빈 값) | 전송 실패. status는 raw와 동일 취급 |

- typed 제출의 status ≥ 400은 `InternalFailure`로 보고한다. 현행 계약에서는
  이때 응답 body가 노출되지 않는다 — 에러 페이로드가 필요하면 raw response terminator를
  사용한다(개정 후보 [R1](10-revision-candidates.ko.md)).
- blocking terminator의 제공 여부와 이름은 [언어별 인터페이스 §1.4](language-interfaces.ko.md#14-종결자-terminator)가
  정한다. typed response의 body만 필요하면 body 전용 terminator(`fetch`)를 사용한다.

## 4.2 응답 타입

- `RawHttpResponse { status: int, headers: map<string,string>, body: string }`
- `HttpResponse<T> { status: int, headers, body: T, rawBody: string }`
- HEAD 응답과 204는 빈 body. typed 경로에서 빈 성공 응답의 body는 언어의
  "없음" 값(null 등)이다.

## 4.3 헤더 표현

- 응답 헤더 name 조회는 **대소문자 무시와 동등**해야 한다. cpp/java/node는
  소문자 정규화 map, dotnet은 원 표기 보존 + case-insensitive map
  (`OrdinalIgnoreCase`) — 둘 다 계약을 충족한다(2026-07-12 R6 승격으로 cpp의
  대소문자 구분 map 편차 해소).

## 4.4 download(sink) 의미론

- sink는 push형 chunk 콜백이다(바이트 뷰: `string_view` /
  `ReadOnlyMemory<byte>` / `byte[]` / `ByteArray` / `Uint8Array`).
- 누적 크기에 `maxResponseBodySize` 상한을 적용한다.
- **압축 해제를 적용하지 않는다** — 원시 바이트가 그대로 전달된다([8장](08-compression.ko.md)).
- retry에서 제외된다(sink 재생 불가).
- redirect 추적 중 **중간 응답의 body는 sink로 새지 않는다**. 최종 응답만
  sink로 흐른다.

## 4.5 typed 디코드

- JSON 디코드 실패는 `ProtocolError`.
- node는 prototype-pollution 방어로 `__proto__`/`constructor`/`prototype`
  키를 제거한다(언어 고유 보안 규칙, 계약 위반 아님).
