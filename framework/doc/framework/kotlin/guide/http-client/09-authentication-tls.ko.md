[← 목차](README.ko.md)

# 9. 인증과 TLS

## Basic / Bearer

```kotlin
zlinkHttpClient("https://api.internal") { basicAuth("user", "secret") }
zlinkHttpClient("https://api.internal") { bearerToken("eyJhbGci...") }
```

둘 다 `Authorization` 헤더를 설정한다. redirect 시 `Authorization`은 **same-origin
에서만 보존**되고 cross-origin으로는 제거된다([10장](10-redirects-retries-cookies.ko.md)).

## HTTPS / TLS 검증

기본적으로 시스템 신뢰 저장소로 서버 인증서를 검증한다. 테스트 인증서(self-signed)를
신뢰하려면 `trustCertificateFile(path)`로 PEM 인증서를 지정한다. 지정한 인증서는 JVM
기본 신뢰에 **추가**되며(대체 아님), hostname 검증은 항상 수행된다
([공통 spec 7.2](../../../common/spec/http-client/07-auth-tls-proxy.ko.md)).

```kotlin
zlinkHttpClient("https://localhost:8443") {
    trustCertificateFile("test-ca.pem")
}
```

## mTLS client certificate

```kotlin
zlinkHttpClient("https://mtls.internal") {
    clientCertificateFile("client-cert.pem", "client-key.pem")
}
```

PEM 인증서(`X.509`)와 PKCS#8 키를 client 인증서로 구성한다.

[다음: Redirect · Retry · Cookie →](10-redirects-retries-cookies.ko.md)
