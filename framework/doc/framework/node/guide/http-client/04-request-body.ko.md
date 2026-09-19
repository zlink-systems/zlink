---
title: "요청 본문 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/http-client/04-request-body.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 요청 본문

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 요청 만들기](03-making-requests.ko.md) | [다음: 응답 받기](05-handling-responses.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/http-client/04-request-body.ko.md) · [C#/.NET](../../../dotnet/guide/http-client/04-request-body.ko.md) · [Java](../../../java/guide/http-client/04-request-body.ko.md) · [Kotlin](../../../kotlin/guide/http-client/04-request-body.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    JSON을 기본 요청 본문으로 보내고, form·multipart가 필요한 API에 맞는
    body 종류를 선택할 수 있다.

request body는 POST·PUT·PATCH처럼 내용을 보내는 요청에 붙는다. HTTP client는 DTO를 JSON으로 직렬화하는 typed JSON body를 기본 경로로 둔다.

## 1. JSON body 보내기

typed JSON body는 application DTO를 `application/json`으로 보낸다. 응답을 받는 방식은 body의 형식과 독립적이므로 typed·raw·body만 받는 응답 중 필요한 형태를 고른다.

<!-- diagram: http-client-request-body -->
```mermaid
flowchart LR
    Dto[Application DTO] --> Json[JSON body] --> Request
```

```typescript
--8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-json-body"
```

## 2. form과 multipart 선택

form은 짧은 name·value 쌍을 `application/x-www-form-urlencoded`로 보내는 API에 맞는다. multipart는 text field와 file field를 한 요청에 넣는 `multipart/form-data` 형식이다. form field는 누적하고, multipart도 field와 file을 누적해 하나의 body를 만든다.

!!! info "body 종류는 하나만 선택한다"

    typed JSON, raw content, stream, form, multipart를 한 요청에 섞으면 요청을 보내지 않고
    `protocolError`로 끝난다.

## 3. raw와 streaming body의 경계

임의 content type의 raw body와 byte stream을 공급하는 streaming upload는 JSON·form·multipart와 다른 경로다. raw body의 응답 처리는 [응답 처리 규칙](10-response-rules.ko.md)을, streaming upload는 [streaming](07-streaming.ko.md)을 따른다.

## 다음 장

[응답 받기](05-handling-responses.ko.md)에서 JSON body를 해석한 응답, raw 응답, body만 받는 응답을 고른다.
