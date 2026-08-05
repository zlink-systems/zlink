# 7. Auth · TLS · Proxy

> [Common contract table of contents](README.en.md)

## 7.1 Auth

- `basicAuth(user, password)` → `Authorization: Basic <base64(user:password)>`
- `bearerToken(token)` → `Authorization: Bearer <token>`
- A per-request `header("authorization", ...)` overrides the client
  setting.
- The `Authorization` removal rule on cross-origin redirect is
  [Chapter 6 §6.1](06-redirect-retry-cookie.en.md).

## 7.2 TLS

- `trustCertificateFile(path)` — **adds** a PEM certificate to the
  trust list. Doesn't replace the system root. For a private CA/
  self-signed server verification.
- `clientCertificateFile(certPath, keyPath)` — mTLS. PEM certificate +
  PKCS#8 key.
- Hostname verification is always performed. An option to turn off
  verification isn't provided.
- Language mapping: dotnet `SslClientAuthenticationOptions`, java
  `SSLContext` (TrustManager/KeyManager), node undici
  `Agent.connect.{ca,cert,key}`, C++ OpenSSL (an HTTPS request is
  `NotConfigured` if OpenSSL isn't in the build).

## 7.3 Proxy

- `proxy(url)` — only allows an **`http://` proxy** (an `https://`
  proxy URL is rejected by builder validation).
- For a plaintext target: sends an absolute-form request to the proxy.
- For an https target: opens a **CONNECT tunnel** and TLS-handshakes
  on top of it. It's end-to-end TLS, and the proxy can't see the
  content.
- `proxyBasicAuth(user, password)` → `Proxy-Authorization: Basic`. The
  proxy auth credential **must not leak to the target server** (only
  carried on the CONNECT tunnel request; node passes it as undici
  `ProxyAgent`'s `token`).
- A CONNECT rejection (407, etc.) is `Unavailable`.
- The proxy is included in the pool key, so proxy presence doesn't
  contaminate connection reuse.
