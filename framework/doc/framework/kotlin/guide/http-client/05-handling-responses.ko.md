---
title: "응답 받기 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/http-client/05-handling-responses.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 응답 받기

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 요청 본문](04-request-body.ko.md) | [다음: 인증·TLS·Proxy](06-auth-tls-proxy.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/http-client/05-handling-responses.ko.md) · [C#/.NET](../../../dotnet/guide/http-client/05-handling-responses.ko.md) · [Java](../../../java/guide/http-client/05-handling-responses.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/http-client/05-handling-responses.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    JSON으로 읽은 body와 HTTP 원문 정보를 목적에 맞게 받을 수 있다. 이 장의 코드는 각 언어의 `HttpClient` tutorial에서 가져왔다.

HTTP client는 같은 요청 builder에서 응답을 받는 형태만 바꾼다. typed 응답은 status와 header, JSON으로 읽은 body를 함께 담는다. raw 응답은 status와 header, JSON으로 읽기 전 body를 담는다. status별 처리 규칙은 [응답 처리 규칙](10-response-rules.ko.md)에서 다룬다.

## 1. Typed 응답을 기본으로 사용한다

성공한 JSON API에는 typed 응답이 기본이다. HTTP status가 400 이상이면 typed 응답은 예외로 끝나므로 오류 body가 필요하면 raw 응답을 선택한다. 종결자(terminator)는 모은 요청을 제출하고 받을 응답 형태를 정하는 마지막 호출이다.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-request-response.html"
        title="http client request response" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-request-response.html" target="_blank">↗ 크게 보기</a></p>

응답 형태는 전송한 요청을 바꾸지 않는다. 호출자가 status와 header를 보존할지, JSON body만 남길지를 정한다.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-response-kinds"
```

예제는 같은 player 응답을 typed 응답, raw 응답, body만 받는 값으로 읽어 status·content type·nickname을 각각 확인한다.

## 2. Raw 응답으로 HTTP 정보를 읽는다

4xx와 5xx도 전송이 성공하면 raw 응답으로 반환하므로, API의 오류 body나 상태 코드에 따라 분기할 때 사용한다.

body만 필요하면 body만 받는 종결자를 사용한다. 이 형태는 성공 JSON의 값만 호출 흐름에 남기며 status와 header는 반환하지 않는다.

!!! note "응답 형태를 먼저 고른다"

    typed 응답은 일반 JSON API에, raw 응답은 HTTP 상태와 오류 body를 직접 처리하는 경로에, body만 받는 응답은 body 외 정보가 필요 없는 성공 경로에 맞는다.

## 3. 압축 응답을 투명하게 읽는다

압축 응답을 보내는 API를 일반 JSON 응답 흐름으로 다룰 때는 `compression`을 켠 client를 사용한다. 세부 동작은 [응답 처리 규칙](10-response-rules.ko.md)에서 다룬다.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-compressed-response"
```

예제는 압축을 켠 client가 room 응답을 status 200의 일반 응답으로 돌려준 결과를 확인한다.

## 4. 다음 장

인증 정보, 인증서, proxy 설정은 [인증·TLS·Proxy](06-auth-tls-proxy.ko.md)에서 다룬다.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
