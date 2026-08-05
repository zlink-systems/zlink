[← 목차](README.ko.md)

# 11. Proxy

## HTTP proxy

`Proxy(url)`로 HTTP proxy를 지정한다. URL은 `http://`로 시작해야 한다.

```csharp
ZLinkHttpClient.Create("https://api.internal")
    .Proxy("http://proxy.internal:3128")
    .Build();
```

## proxy 인증

```csharp
ZLinkHttpClient.Create("https://api.internal")
    .Proxy("http://proxy.internal:3128")
    .ProxyBasicAuth("proxy-user", "proxy-secret")
    .Build();
```

`ProxyBasicAuth`는 `Proxy-Authorization: Basic ...` 헤더를 구성한다.

## 동작 의미론

[공통 spec 7.3](../../../common/spec/http-client/07-auth-tls-proxy.ko.md)을 따른다.

- 평문(`http://`) 대상은 absolute-form 요청으로 proxy에 전달된다.
- `https://` 대상은 **CONNECT tunnel**을 연 뒤 그 위에서 TLS handshake를
  수행한다. TLS는 end-to-end이므로 proxy는 요청/응답 내용을 볼 수 없다.
- proxy 인증 정보는 proxy에게만 실리고 **대상 서버로 새지 않는다**.
- proxy가 CONNECT를 거부하면(407 등) `Unavailable`로 보고된다.
- connection pool 키에 proxy가 포함되어 proxy 유무가 연결 재사용을
  오염시키지 않는다.

[다음: 압축 →](12-compression.ko.md)
