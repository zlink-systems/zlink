---
title: "응답 처리 규칙 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/http-client/10-response-rules.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 응답 처리 규칙

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: Redirect·Retry·Cookie](09-redirect-retry-cookie.ko.md) | [다음: 오류 처리](11-error-handling.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/http-client/10-response-rules.ko.md) · [C#/.NET](../../../dotnet/guide/http-client/10-response-rules.ko.md) · [Java](../../../java/guide/http-client/10-response-rules.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/http-client/10-response-rules.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    응답 형태별 status 처리, JSON decode와 body 크기 한도, 압축 해제의 경계를 구분할 수 있다.

같은 HTTP 응답도 무엇을 받아야 하는지에 따라 처리 경로가 달라진다. typed 응답은 JSON body를
application 타입으로 읽은 결과이고, raw 응답은 status·header·원본 body를 함께 보존한 결과다.
오류 status의 payload나 header를 직접 판단해야 하면 raw 응답을 사용한다.

## 1. status에 따른 응답 경로

| 응답 형태 | 2xx·3xx | 4xx·5xx | body |
|---|---|---|---|
| raw 응답 | 성공으로 반환 | 성공으로 반환 | 원본 body를 제공한다 |
| typed 응답 | 성공으로 반환 | `InternalFailure` | JSON decode한 값을 제공한다 |
| body만 받는 응답 | 성공으로 반환 | `InternalFailure` | decode한 body만 제공한다 |
| download sink | 성공으로 반환 | raw 응답과 같이 반환 | 최종 응답 byte만 sink에 전달한다 |

typed 응답과 body만 받는 응답은 status가 400 이상이면 body를 노출하지 않는다. 오류 payload가 필요하면
raw 응답을 선택한다. HEAD와 204의 성공 응답 body는 비어 있으며 typed 경로에서는 언어의 없음 값이 된다.

## 2. JSON decode 실패와 body 한도

typed 응답이 JSON body를 대상 타입으로 읽지 못하면 `protocolError`가 발생한다. 이 실패는 HTTP
status와 별개이므로, 성공 status라도 응답 형식이 계약과 다르면 typed 결과를 만들지 않는다.

일반 응답 body의 기본 상한은 16 MiB다. 누적한 body가 상한을 넘으면 `Rejected`가 발생한다. download
sink도 누적 크기에 같은 상한을 적용하지만 raw byte를 받는 경로이므로 다음 절의 압축 해제를 적용하지
않는다.

## 3. 압축 응답을 읽는 방법

압축은 기본으로 꺼져 있다. tutorial은 압축 옵션을 켜고 typed 응답의 `content-encoding` header가
사라진 것을 확인한다.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-compressed-response"
```

```console
compressed response: 200 encoding-removed true
```

압축을 켜면 요청에 `Accept-Encoding: gzip, deflate`를 추가한다. `gzip`과 `deflate` 응답은 body를
투명하게 해제하고 `content-encoding`과 관련 length header를 응답 header에서 제거한다. 따라서 typed와
raw 응답 모두 압축이 없었던 것처럼 body를 읽는다. 해제한 body에도 16 MiB 상한을 적용하므로 압축된
작은 응답이 큰 body로 풀려도 `Rejected`로 끝난다. 손상된 압축 body는 `protocolError`다.

**download sink에는 압축 해제를 적용하지 않는다.** sink는 서버가 보낸 압축 byte를 그대로 받는다.
해제한 content가 필요한 경우 download가 아닌 일반 응답을 선택한다.

## 4. raw body를 고르는 경우

raw 응답은 status·header·body를 모두 보존하므로 API의 오류 payload, content type, redirect를 따르지
않은 status를 직접 처리할 때 적합하다. 반대로 JSON 구조가 안정되어 있고 성공 body만 필요하면 typed
응답 또는 body만 받는 응답이 decode와 status 검사를 함께 수행한다.

## 5. 자주 발생하는 문제

| 증상 | 원인 |
|---|---|
| 404 또는 500의 payload를 typed 응답에서 읽지 못한다 | typed와 body만 받는 경로는 status 400 이상을 `InternalFailure`로 처리한다. |
| 성공 status인데 typed 호출이 실패한다 | JSON body가 대상 타입으로 decode되지 않아 `protocolError`가 발생했다. |
| 작은 gzip 응답이 body 제한을 넘는다 | 한도는 압축을 해제한 body 크기에 적용된다. |
| download 파일이 gzip byte처럼 보인다 | download sink에는 압축 해제를 적용하지 않는다. |

## 6. 다음 장

- 압축되지 않은 stream을 내려받거나 올리는 방법 — [Streaming](07-streaming.ko.md)
- 실패 kind와 새 operation 판단 — [오류 처리](11-error-handling.ko.md)
