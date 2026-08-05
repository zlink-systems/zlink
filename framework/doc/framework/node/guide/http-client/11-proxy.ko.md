[← 목차](README.ko.md)

# 11. Proxy

## HTTP proxy

`proxy(url)`로 HTTP proxy를 지정한다. URL은 `http://`로 시작해야 한다. 내부적으로
undici `ProxyAgent`로 dispatcher를 구성한다.

```ts
ZLinkHttpClient.create('https://api.internal')
  .proxy('http://proxy.internal:3128')
  .build();
```

## proxy 인증

```ts
ZLinkHttpClient.create('https://api.internal')
  .proxy('http://proxy.internal:3128')
  .proxyBasicAuth('proxy-user', 'proxy-secret')
  .build();
```

`proxyBasicAuth`는 `ProxyAgent`의 `token`(`Proxy-Authorization`)으로 전달된다. 요청 헤더로
싣지 않으므로 CONNECT tunnel에서 target에 노출되지 않는다.

[다음: 압축 →](12-compression.ko.md)
