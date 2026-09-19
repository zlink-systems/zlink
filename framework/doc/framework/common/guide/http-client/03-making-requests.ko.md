# 요청 만들기

!!! info "이 장을 읽고 나면"

    HTTP method와 path를 고르고, query·header·요청별 timeout으로 한 요청을
    외부 API의 요구에 맞게 구성할 수 있다.

request builder는 client에서 method를 선택할 때 생기며, 종결자로 응답을 요청하기 전까지 요청의 모양을 누적한다. 종결자(terminator)는 builder 구성을 끝내고 실제 요청을 제출해 응답 형태를 정하는 마지막 호출이다. path는 base URL 뒤에 붙는 `/`로 시작하는 경로다.

## 1. method와 path 선택

HTTP client는 GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS의 일곱 method를 제공한다. GET은 읽기에, POST는 새 작업이나 resource 생성에 사용한다. C++에서는 keyword와 겹치는 DELETE를 `delete_`로 표기한다.

path가 `/`로 시작하지 않으면 요청을 보내지 않고 `ProtocolError`로 끝난다. base URL과 path를 한 요청 안에서 섞지 않고, 다른 base URL이 필요하면 별도 client 또는 one-shot을 사용한다.

## 2. query·header·timeout 추가

query는 name과 value를 URL-encoding하여 누적한다. header는 그 요청에만 적용되며 같은 이름의 client 기본 header보다 우선한다. 요청별 timeout은 client의 기본 timeout을 그 요청에서만 대체한다.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-request-shaping.html"
        title="http client request shaping" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-request-shaping.html" target="_blank">↗ 크게 보기</a></p>

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-request-shaping"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-request-shaping"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-request-shaping"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-request-shaping"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-request-shaping"
    ```

이 코드는 일반 API와 관리 API에 서로 다른 base URL을 사용하고, 요청별 header·timeout·query를 더한 뒤 typed 응답에서 status와 변경된 weight를 읽는다.

## 3. 요청마다 달라지는 값

요청마다 달라지는 인증 정보, 추적 ID, query와 timeout은 request builder에 둔다. 여러 요청에서 항상 같은 header나 인증 정보는 client를 만들 때 설정한다. timeout은 한 시도의 시간 경계이며, retry를 설정하면 각 시도에 적용된다. 재시도와 응답 상태 규칙은 [redirect·retry·cookie](09-redirect-retry-cookie.ko.md)와 [응답 처리 규칙](10-response-rules.ko.md)에서 다룬다.

## 다음 장

[요청 본문](04-request-body.ko.md)에서 JSON, form, multipart body를 붙인다.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
