[← 목차](README.ko.md)

# 9. 인증과 TLS

## Basic / Bearer

```ts
ZLinkHttpClient.create('https://api.internal').basicAuth('user', 'secret').build();
ZLinkHttpClient.create('https://api.internal').bearerToken('eyJhbGci...').build();
```

둘 다 `Authorization` 헤더를 설정한다. redirect 시 `Authorization`은 **same-origin
에서만 보존**되고 cross-origin으로는 제거된다([10장](10-redirects-retries-cookies.ko.md)).

## HTTPS / TLS 검증

기본적으로 시스템 신뢰 저장소로 서버 인증서를 검증한다. 테스트 인증서(self-signed)를
신뢰하려면 `trustCertificateFile(path)`로 PEM 인증서를 지정한다. 내부적으로 undici
`Agent`의 `connect.ca`로 신뢰 anchor에 추가한다.

```ts
ZLinkHttpClient.create('https://localhost:8443')
  .trustCertificateFile('test-ca.pem')
  .build();
```

## mTLS client certificate

```ts
ZLinkHttpClient.create('https://mtls.internal')
  .clientCertificateFile('client-cert.pem', 'client-key.pem')
  .build();
```

PEM 인증서와 키로 undici `Agent`의 `connect.cert`/`connect.key`를 구성한다.

[다음: Redirect · Retry · Cookie →](10-redirects-retries-cookies.ko.md)
