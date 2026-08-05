[← Table Of Contents](README.en.md)

# 9. Authentication And TLS

## Basic / Bearer

```kotlin
zlinkHttpClient("https://api.internal") { basicAuth("user", "secret") }
zlinkHttpClient("https://api.internal") { bearerToken("eyJhbGci...") }
```

Both set the `Authorization` header. On redirect, `Authorization` is **preserved only same-origin**
and is removed cross-origin ([Chapter 10](10-redirects-retries-cookies.en.md)).

## HTTPS / TLS Verification

By default, server certificates are verified against the system trust store. To trust a test
(self-signed) certificate, specify a PEM certificate with `trustCertificateFile(path)`. The
specified certificate is **added** to the JVM's default trust (not replaced), and hostname
verification always runs ([Common Spec 7.2](../../../common/spec/http-client/07-auth-tls-proxy.en.md)).

```kotlin
zlinkHttpClient("https://localhost:8443") {
    trustCertificateFile("test-ca.pem")
}
```

## mTLS Client Certificate

```kotlin
zlinkHttpClient("https://mtls.internal") {
    clientCertificateFile("client-cert.pem", "client-key.pem")
}
```

Configures the PEM certificate (`X.509`) and PKCS#8 key as the client certificate.

[Next: Redirect · Retry · Cookie →](10-redirects-retries-cookies.en.md)
