[← Table Of Contents](README.en.md)

# 9. Authentication And TLS

## Basic / Bearer

```java
ZLinkHttpClient.create("https://api.internal").basicAuth("user", "secret").build();
ZLinkHttpClient.create("https://api.internal").bearerToken("eyJhbGci...").build();
```

Both set the `Authorization` header. On redirect, `Authorization` is **preserved only same-origin**
and is removed cross-origin ([Chapter 10](10-redirects-retries-cookies.en.md)).

## HTTPS / TLS Verification

By default, server certificates are verified against the system trust store. To trust a test
(self-signed) certificate, specify a PEM certificate with `trustCertificateFile(path)`. Internally,
this configures the `SSLContext` with a composite `TrustManager` that checks both the JVM's default
trust and the specified certificate — that is, it's **added** to the default trust (not replaced), so
public-CA HTTPS keeps working. Hostname verification always runs, with no option to turn it off
([Common Spec 7.2](../../../common/spec/http-client/07-auth-tls-proxy.en.md)).

```java
ZLinkHttpClient.create("https://localhost:8443")
    .trustCertificateFile("test-ca.pem")
    .build();
```

## mTLS Client Certificate

```java
ZLinkHttpClient.create("https://mtls.internal")
    .clientCertificateFile("client-cert.pem", "client-key.pem")
    .build();
```

Configures a `KeyManager` with the PEM certificate (`X.509`) and PKCS#8 key, and loads it into the
`SSLContext`.

[Next: Redirect · Retry · Cookie →](10-redirects-retries-cookies.en.md)
