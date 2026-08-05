[← Table Of Contents](README.en.md)

# 9. Authentication And TLS

## Basic / Bearer

```csharp
ZLinkHttpClient.Create("https://api.internal").BasicAuth("user", "secret").Build();
ZLinkHttpClient.Create("https://api.internal").BearerToken("eyJhbGci...").Build();
```

Both set the `Authorization` header. On redirect, `Authorization` is **preserved only same-origin**
and is removed cross-origin ([Chapter 10](10-redirects-retries-cookies.en.md)).

## HTTPS / TLS Verification

By default, server certificates are verified against the system trust store. To trust a test
(self-signed) certificate, specify a PEM certificate with `TrustCertificateFile(path)`. The
specified certificate is **added** to the system trust (not replaced) — public-CA HTTPS keeps
working. Hostname verification always runs, with no option to turn it off
([Common Spec 7.2](../../../common/spec/http-client/07-auth-tls-proxy.en.md)).

```csharp
ZLinkHttpClient.Create("https://localhost:8443")
    .TrustCertificateFile("test-ca.pem")
    .Build();
```

## mTLS Client Certificate

```csharp
ZLinkHttpClient.Create("https://mtls.internal")
    .ClientCertificateFile("client-cert.pem", "client-key.pem")
    .Build();
```

Configures a client certificate from the PEM certificate and key, and loads it into
`SslClientAuthenticationOptions`.

[Next: Redirect · Retry · Cookie →](10-redirects-retries-cookies.en.md)
