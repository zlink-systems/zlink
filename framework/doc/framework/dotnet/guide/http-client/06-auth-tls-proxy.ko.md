---
title: "인증·TLS·Proxy · C#/.NET"
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
다른 언어로 보기 — [C++](../../../cpp/guide/http-client/06-auth-tls-proxy.ko.md) · **C#/.NET** · [Java](../../../java/guide/http-client/06-auth-tls-proxy.ko.md) · [Kotlin](../../../kotlin/guide/http-client/06-auth-tls-proxy.ko.md) · [Node/TypeScript](../../../node/guide/http-client/06-auth-tls-proxy.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    인증 방식과 TLS·mTLS·proxy builder 옵션의 역할을 구분할 수 있다. Basic 인증 예제는 각 언어의 `HttpClient` tutorial에서 가져왔다.

인증과 전송 보안은 client를 만들 때 정한다. 요청마다 다른 인증 header가 필요하면 request builder의 header가 client 설정을 덮는다. 이 tutorial은 평문 HTTP로 동작하고 proxy를 사용하지 않는다.

!!! warning "이 tutorial은 평문으로 동작한다"

    TLS, mTLS, proxy 옵션은 배포 환경의 endpoint에 맞춰 설정한다. tutorial 코드에는 이 옵션을 넣지 않는다.

## 1. Basic 또는 Bearer 인증을 설정한다

`BasicAuth`는 user와 password로 `Authorization: Basic` header를 만든다. `BearerToken`은 token으로 `Authorization: Bearer` header를 만든다. 요청별 `Authorization` header는 이 client 설정보다 우선한다.

```csharp
--8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-basic-auth"
```

## 2. 신뢰할 인증서를 추가한다

`TrustCertificateFile`은 PEM 인증서를 시스템 신뢰 목록에 추가한다. 사설 CA 또는 자체 서명 서버를 검증할 때 사용하며, 시스템 root를 대체하지 않는다. hostname 검증을 끄는 옵션은 제공하지 않는다.

## 3. mTLS client 인증서를 설정한다

`ClientCertificateFile`은 PEM client 인증서와 PKCS#8 key를 설정한다. 서버가 client 인증서를 요구하는 HTTPS endpoint에만 사용한다.

## 4. HTTP proxy를 설정한다

`Proxy`는 `http://` proxy URL만 받는다. HTTPS 대상은 proxy에 CONNECT tunnel을 열고 그 안에서 TLS를 수행하므로 proxy가 대상 요청 내용을 읽지 못한다. `ProxyBasicAuth`는 proxy에만 Basic 인증을 보내며 대상 서버로 전달하지 않는다.

## 5. 다음 장

큰 body를 누적하지 않고 보내거나 받는 경로는 [Streaming](07-streaming.ko.md)에서 다룬다.
