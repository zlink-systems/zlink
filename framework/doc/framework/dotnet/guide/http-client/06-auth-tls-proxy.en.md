---
title: "Authentication, TLS, and Proxy · C#/.NET"
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
View in another language — [C++](../../../cpp/guide/http-client/06-auth-tls-proxy.en.md) · **C#/.NET** · [Java](../../../java/guide/http-client/06-auth-tls-proxy.en.md) · [Kotlin](../../../kotlin/guide/http-client/06-auth-tls-proxy.en.md) · [Node/TypeScript](../../../node/guide/http-client/06-auth-tls-proxy.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can distinguish the roles of authentication, TLS, mTLS, and proxy builder options. The Basic authentication example comes from each language's `HttpClient` tutorial.

Authentication and transport security are selected while creating a client. A request-specific authentication header overrides client configuration when credentials differ for one request. This tutorial runs over plain HTTP and does not use a proxy.

!!! warning "This tutorial uses plain HTTP"

    Configure TLS, mTLS, and proxy options for the deployed endpoint. The tutorial code does not include these options.

## 1. Configure Basic or Bearer Authentication

`BasicAuth` creates an `Authorization: Basic` header from a user and password. `BearerToken` creates an `Authorization: Bearer` header from a token. A request-specific `Authorization` header takes precedence over this client configuration.

```csharp
--8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-basic-auth"
```

## 2. Add a Trusted Certificate

`TrustCertificateFile` adds a PEM certificate to the system trust store. Use it to validate a private CA or self-signed server; it does not replace system roots. No option disables hostname validation.

## 3. Configure an mTLS Client Certificate

`ClientCertificateFile` configures a PEM client certificate and a PKCS#8 key. Use it only for an HTTPS endpoint that requires client authentication.

## 4. Configure an HTTP Proxy

`Proxy` accepts an `http://` proxy URL only. For an HTTPS target, the client opens a CONNECT tunnel through the proxy and performs TLS inside it, so the proxy cannot read the target request content. `ProxyBasicAuth` sends Basic credentials only to the proxy, never to the target server.

## 5. Next Chapter

[Streaming](07-streaming.en.md) covers paths that send or receive a large body without accumulating it.
