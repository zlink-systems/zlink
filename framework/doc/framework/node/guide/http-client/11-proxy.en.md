[← Table Of Contents](README.en.md)

# 11. Proxy

## HTTP Proxy

Specify an HTTP proxy with `proxy(url)`. The URL must start with `http://`. Internally, this
configures the dispatcher with undici's `ProxyAgent`.

```ts
ZLinkHttpClient.create('https://api.internal')
  .proxy('http://proxy.internal:3128')
  .build();
```

## Proxy Authentication

```ts
ZLinkHttpClient.create('https://api.internal')
  .proxy('http://proxy.internal:3128')
  .proxyBasicAuth('proxy-user', 'proxy-secret')
  .build();
```

`proxyBasicAuth` is passed as the `ProxyAgent`'s `token` (`Proxy-Authorization`). It's not carried
as a request header, so it's not exposed to the target through the CONNECT tunnel.

[Next: Compression →](12-compression.en.md)
