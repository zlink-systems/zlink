---
title: "Authentication, TLS, and Proxy · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/06-auth-tls-proxy.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Authentication, TLS, and Proxy

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Handling Responses](05-handling-responses.en.md) | [Next: Streaming](07-streaming.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/06-auth-tls-proxy.en.md) · [C#/.NET](../../../dotnet/guide/http-client/06-auth-tls-proxy.en.md) · [Java](../../../java/guide/http-client/06-auth-tls-proxy.en.md) · [Kotlin](../../../kotlin/guide/http-client/06-auth-tls-proxy.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    This chapter distinguishes credentials, certificates, and proxy builder options. The Basic authentication example comes from each language's `HttpClient` tutorial.

Authentication and transport security are selected while creating a client. TLS encrypts transport to an HTTPS target and validates its server certificate. mTLS also validates a client certificate. A proxy relays the client's HTTP requests to the target API. A request-specific authentication header overrides client configuration when credentials differ for one request.

!!! warning "This tutorial uses plain HTTP"

    TLS, mTLS, and proxy options belong to the deployed endpoint. The tutorial code omits them.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-auth-tls-proxy-en.html"
        title="http client auth tls proxy" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-auth-tls-proxy-en.html" target="_blank">↗ 크게 보기</a></p>

Target credentials attach to the request header, while proxy credentials attach only to the proxy leg. Certificate validation and mTLS apply to the TLS leg for both a direct connection and a CONNECT tunnel.

## 1. Basic and Bearer Authentication

`basicAuth` creates an `Authorization: Basic` header from a user and password. `bearerToken` creates an `Authorization: Bearer` header from a token. A request-specific `Authorization` header takes precedence over this client configuration.

```typescript
--8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-basic-auth"
```

The example contrasts a 401 response without credentials with a 200 response from the same path using Basic authentication.

## 2. Trusted Certificates

`trustCertificateFile` adds a PEM certificate to the system trust store. It validates a private CA or self-signed server without replacing system roots. No option disables hostname validation.

## 3. An mTLS Client Certificate

`clientCertificateFile` configures a PEM client certificate and a PKCS#8 key. It applies to an HTTPS endpoint that requires client authentication.

## 4. An HTTP Proxy

`proxy` accepts an `http://` proxy URL only. For an HTTPS target, the client opens a CONNECT tunnel through the proxy and performs TLS inside it, so the proxy cannot read the target request content. `proxyBasicAuth` sends Basic credentials only to the proxy, never to the target server.

## 5. Next Chapter

[Streaming](07-streaming.en.md) covers paths that send or receive a large body without accumulating it.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
