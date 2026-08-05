[← Table Of Contents](README.en.md)

# 11. Proxy

## HTTP Proxy

Specify an HTTP proxy with `Proxy(url)`. The URL must start with `http://`.

```csharp
ZLinkHttpClient.Create("https://api.internal")
    .Proxy("http://proxy.internal:3128")
    .Build();
```

## Proxy Authentication

```csharp
ZLinkHttpClient.Create("https://api.internal")
    .Proxy("http://proxy.internal:3128")
    .ProxyBasicAuth("proxy-user", "proxy-secret")
    .Build();
```

`ProxyBasicAuth` builds a `Proxy-Authorization: Basic ...` header.

## Behavioral Semantics

Follows [Common Spec 7.3](../../../common/spec/http-client/07-auth-tls-proxy.en.md).

- A plaintext (`http://`) target is delivered to the proxy as an absolute-form request.
- An `https://` target opens a **CONNECT tunnel** first, then performs the TLS handshake on top of
  it. Since TLS is end-to-end, the proxy can't see the request/response content.
- Proxy credentials are carried only to the proxy and **don't leak to the target server**.
- If the proxy rejects the CONNECT (407, etc.), it's reported as `Unavailable`.
- The proxy is part of the connection pool key, so the presence of a proxy doesn't contaminate
  connection reuse.

[Next: Compression →](12-compression.en.md)
