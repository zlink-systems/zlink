---
title: "인증·TLS·Proxy · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/http-client/06-auth-tls-proxy.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 인증·TLS·Proxy

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 응답 받기](05-handling-responses.ko.md) | [다음: Streaming](07-streaming.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/http-client/06-auth-tls-proxy.ko.md) · [C#/.NET](../../../dotnet/guide/http-client/06-auth-tls-proxy.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/http-client/06-auth-tls-proxy.ko.md) · [Node/TypeScript](../../../node/guide/http-client/06-auth-tls-proxy.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    인증 정보와 인증서, proxy builder 옵션의 역할을 구분할 수 있다. Basic 인증 예제는 각 언어의 `HttpClient` tutorial에서 가져왔다.

인증과 전송 보안은 client를 만들 때 정한다. TLS는 client와 HTTPS 대상 사이의 전송을 암호화하고 서버 인증서를 검증한다. mTLS는 서버 검증에 더해 client 인증서도 확인한다. proxy는 client의 HTTP 요청을 대상 API로 중계한다. 요청마다 다른 인증 header가 필요하면 request builder의 header가 client 설정을 덮는다.

!!! warning "이 tutorial은 평문으로 동작한다"

    TLS, mTLS, proxy 옵션은 배포 환경의 endpoint에 맞춰 설정한다. tutorial 코드에는 이 옵션을 넣지 않는다.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-auth-tls-proxy.html"
        title="http client auth tls proxy" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-auth-tls-proxy.html" target="_blank">↗ 크게 보기</a></p>

대상 인증은 요청 header에 붙고, proxy 인증은 proxy 구간에만 붙는다. HTTPS의 인증서 검증과 mTLS는 direct 연결과 CONNECT tunnel 뒤의 TLS 구간에 같은 방식으로 적용된다.

## 1. Basic 또는 Bearer 인증을 설정한다

`basicAuth`는 user와 password로 `Authorization: Basic` header를 만든다. `bearerToken`은 token으로 `Authorization: Bearer` header를 만든다. 요청별 `Authorization` header는 이 client 설정보다 우선한다.

```java
--8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-basic-auth"
```

예제는 인증이 없는 요청의 401과 같은 경로에 Basic 인증을 붙인 요청의 200을 나란히 확인한다.

## 2. 신뢰할 인증서를 추가한다

`trustCertificateFile`은 PEM 인증서를 시스템 신뢰 목록에 추가한다. 사설 CA 또는 자체 서명 서버를 검증할 때 사용하며, 시스템 root를 대체하지 않는다. hostname 검증을 끄는 옵션은 제공하지 않는다.

## 3. mTLS client 인증서를 설정한다

`clientCertificateFile`은 PEM client 인증서와 PKCS#8 key를 설정한다. 서버가 client 인증서를 요구하는 HTTPS endpoint에만 사용한다.

## 4. HTTP proxy를 설정한다

`proxy`는 `http://` proxy URL만 받는다. HTTPS 대상은 proxy에 CONNECT tunnel을 열고 그 안에서 TLS를 수행하므로 proxy가 대상 요청 내용을 읽지 못한다. `proxyBasicAuth`는 proxy에만 Basic 인증을 보내며 대상 서버로 전달하지 않는다.

## 5. 다음 장

큰 body를 누적하지 않고 보내거나 받는 경로는 [Streaming](07-streaming.ko.md)에서 다룬다.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
