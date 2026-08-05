[← Table Of Contents](README.en.md)

# 9. Authentication And TLS

## Basic / Bearer

```ts
ZLinkHttpClient.create('https://api.internal').basicAuth('user', 'secret').build();
ZLinkHttpClient.create('https://api.internal').bearerToken('eyJhbGci...').build();
```

Both set the `Authorization` header. On redirect, `Authorization` is **preserved only same-origin**
and is removed cross-origin ([Chapter 10](10-redirects-retries-cookies.en.md)).

## HTTPS / TLS Verification

By default, server certificates are verified against the system trust store. To trust a test
(self-signed) certificate, specify a PEM certificate with `trustCertificateFile(path)`. Internally
this adds it as a trust anchor through undici's `Agent` `connect.ca`.

```ts
ZLinkHttpClient.create('https://localhost:8443')
  .trustCertificateFile('test-ca.pem')
  .build();
```

## mTLS Client Certificate

```ts
ZLinkHttpClient.create('https://mtls.internal')
  .clientCertificateFile('client-cert.pem', 'client-key.pem')
  .build();
```

Configures undici's `Agent` `connect.cert`/`connect.key` with the PEM certificate and key.

[Next: Redirect · Retry · Cookie →](10-redirects-retries-cookies.en.md)
