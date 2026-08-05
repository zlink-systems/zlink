[← 목차](README.ko.md)

# 11. Proxy

## HTTP proxy

`proxy(url)`로 HTTP proxy를 지정한다. URL은 `http://`로 시작해야 한다. 내부적으로
`ProxySelector`로 `HttpClient`에 구성된다.

```java
ZLinkHttpClient.create("https://api.internal")
    .proxy("http://proxy.internal:3128")
    .build();
```

## proxy 인증

```java
ZLinkHttpClient.create("https://api.internal")
    .proxy("http://proxy.internal:3128")
    .proxyBasicAuth("proxy-user", "proxy-secret")
    .build();
```

`proxyBasicAuth`는 `Proxy-Authorization: Basic ...` 헤더를 구성한다.

## 동작 의미론

[공통 spec 7.3](../../../common/spec/http-client/07-auth-tls-proxy.ko.md)을 따른다.

- 평문(`http://`) 대상은 absolute-form 요청으로 proxy에 전달된다.
- `https://` 대상은 **CONNECT tunnel**을 연 뒤 그 위에서 TLS handshake를
  수행한다(`java.net.http`가 tunnel을 처리). TLS는 end-to-end이므로 proxy는
  요청/응답 내용을 볼 수 없다.
- proxy 인증 정보는 proxy에게만 실리고 **대상 서버로 새지 않는다**.
- proxy가 CONNECT를 거부하면(407 등) 전송 실패로 보고된다.

[다음: 압축 →](12-compression.ko.md)
