[← 목차](README.ko.md)

# 3. Client 구성

builder는 client 전역 설정을 모은다. `java.net.http` 설정으로 매핑되거나, 의미론이 다른
경우 래퍼에서 직접 처리된다.

## builder 옵션

기본값은 [공통 spec 2장](../../../common/spec/http-client/02-client-builder.ko.md)이 정본이다.

| 옵션 | 효과 | 기본값 | 구현 |
| --- | --- | --- | --- |
| `baseUrl(url)` | 모든 요청의 기준 URL | 없음(필수) | 래퍼 |
| `timeout(Duration)` | 시도당 timeout(요청별 override 가능) | **3000ms** | `HttpRequest.timeout` |
| `defaultHeader(n, v)` | 모든 요청에 붙는 기본 헤더 | 없음 | 래퍼 |
| `basicAuth(u, p)` / `bearerToken(t)` | `Authorization` 헤더 | off | 래퍼 |
| `maxResponseBodySize(bytes)` | 응답 본문 상한(decoded 기준) | **16 MiB** | 래퍼 |
| `trustCertificateFile(path)` | 신뢰 인증서 추가 | 시스템 root | `SSLContext` TrustManager |
| `clientCertificateFile(cert, key)` | mTLS client 인증서 | off | `SSLContext` KeyManager |
| `followRedirects(max)` | redirect 추적(무인자 시 **5회**) | off | **래퍼 redirect 루프** |
| `retry(attempts)` | transport 실패 재시도(총 1+n회 시도) | off | **래퍼 retry 루프** |
| `cookies()` | cookie jar 활성화 | off | **래퍼 cookie jar** |
| `proxy(url)` / `proxyBasicAuth(u, p)` | HTTP proxy | off | `ProxySelector` |
| `compression()` | gzip/deflate 투명 해제 | off | **래퍼 해제** |

## 네이티브 위임 vs 래퍼 구현

`HttpClient`는 `Redirect.NEVER`로 두고(래퍼 루프), cookie manager는 쓰지 않는다(래퍼 jar).
`java.net.http`는 응답을 자동 해제하지 않으므로 래퍼가 `java.util.zip`으로 통제한다.
connection pool·proxy·TLS는 네이티브에 위임한다. `Redirect` enum에는 횟수 개념이 없어
redirect 루프를 래퍼에서 구현한다.

## client 재사용

client는 내부 `HttpClient` 하나를 감싸며 connection pool을 공유한다. **한 번 만들어
재사용**하고 try-with-resources나 `close()`로 정리한다.

## 요청별 timeout override

```java
client.get("/slow-report").timeout(Duration.ofSeconds(30)).submitRaw();
```

[다음: Request 만들기 →](04-making-requests.ko.md)
