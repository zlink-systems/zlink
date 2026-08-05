# 7. 인증 · TLS · Proxy

> [공통 계약 목차](README.ko.md)

## 7.1 인증

- `basicAuth(user, password)` → `Authorization: Basic <base64(user:password)>`
- `bearerToken(token)` → `Authorization: Bearer <token>`
- 요청별 `header("authorization", ...)`가 client 설정을 덮는다.
- redirect cross-origin 시 `Authorization` 제거 규칙은 [6장 §6.1](06-redirect-retry-cookie.ko.md).

## 7.2 TLS

- `trustCertificateFile(path)` — PEM 인증서를 신뢰 목록에 **추가**한다.
  시스템 root를 대체하지 않는다. 사설 CA/자체 서명 서버 검증용.
- `clientCertificateFile(certPath, keyPath)` — mTLS. PEM 인증서 + PKCS#8 키.
- hostname 검증은 항상 수행한다. 검증 끄기 옵션은 제공하지 않는다.
- 언어 매핑: dotnet `SslClientAuthenticationOptions`, java `SSLContext`
  (TrustManager/KeyManager), node undici `Agent.connect.{ca,cert,key}`,
  C++ OpenSSL(빌드에 OpenSSL이 없으면 HTTPS 요청은 `NotConfigured`).

## 7.3 Proxy

- `proxy(url)` — **`http://` proxy만** 허용한다(`https://` proxy URL은
  builder 검증에서 거부).
- 평문 대상: absolute-form 요청을 proxy로 보낸다.
- https 대상: **CONNECT tunnel**을 열고 그 위에서 TLS handshake한다.
  end-to-end TLS이며 proxy는 내용을 볼 수 없다.
- `proxyBasicAuth(user, password)` → `Proxy-Authorization: Basic`.
  proxy 인증 정보는 **대상 서버로 새지 않아야 한다**(CONNECT tunnel 요청에만
  실림. node는 undici `ProxyAgent`의 `token`으로 전달).
- CONNECT 거부(407 등)는 `Unavailable`.
- pool 키에 proxy가 포함되어 proxy 유무가 연결 재사용을 오염시키지 않는다.
