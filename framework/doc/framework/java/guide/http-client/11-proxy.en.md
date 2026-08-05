[← Table Of Contents](README.en.md)

# 11. Proxy

## HTTP Proxy

Specify an HTTP proxy with `proxy(url)`. The URL must start with `http://`. Internally, this is
configured on `HttpClient` via a `ProxySelector`.

```java
ZLinkHttpClient.create("https://api.internal")
    .proxy("http://proxy.internal:3128")
    .build();
```

## Proxy Authentication

```java
ZLinkHttpClient.create("https://api.internal")
    .proxy("http://proxy.internal:3128")
    .proxyBasicAuth("proxy-user", "proxy-secret")
    .build();
```

`proxyBasicAuth` builds a `Proxy-Authorization: Basic ...` header.

## Behavioral Semantics

Follows [Common Spec 7.3](../../../common/spec/http-client/07-auth-tls-proxy.en.md).

- A plaintext (`http://`) target is delivered to the proxy as an absolute-form request.
- An `https://` target opens a **CONNECT tunnel** first, then performs the TLS handshake on top of
  it (`java.net.http` handles the tunnel). Since TLS is end-to-end, the proxy can't see the
  request/response content.
- Proxy credentials are carried only to the proxy and **don't leak to the target server**.
- If the proxy rejects the CONNECT (407, etc.), it's reported as a transport failure.

[Next: Compression →](12-compression.en.md)
