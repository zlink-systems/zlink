[← 목차](README.ko.md)

# 3. Client 구성

builder는 client 전역 설정을 모은다. undici dispatcher 설정으로 매핑되거나, 의미론이
다른 경우 래퍼에서 직접 처리된다.

## builder 옵션

기본값은 [공통 spec 2장](../../../common/spec/http-client/02-client-builder.ko.md)이 정본이다.

| 옵션 | 효과 | 기본값 | 구현 |
| --- | --- | --- | --- |
| `baseUrl(url)` | 모든 요청의 기준 URL | 없음(필수) | 래퍼 |
| `timeout(ms)` | 시도당 timeout(요청별 override 가능) | **3000ms** | `AbortController` |
| `defaultHeader(n, v)` | 모든 요청에 붙는 기본 헤더 | 없음 | 래퍼 |
| `basicAuth(u, p)` / `bearerToken(t)` | `Authorization` 헤더 | off | 래퍼 |
| `maxResponseBodySize(bytes)` | 응답 본문 상한(decoded 기준) | **16 MiB** | 래퍼 |
| `trustCertificateFile(path)` | 신뢰 인증서 추가 | 시스템 root | undici `Agent` `connect.ca` |
| `clientCertificateFile(cert, key)` | mTLS client 인증서 | off | undici `Agent` `connect.cert/key` |
| `followRedirects(max)` | redirect 추적(무인자 시 **5회**) | off | **래퍼 redirect 루프** |
| `retry(attempts)` | transport 실패 재시도(총 1+n회 시도) | off | **래퍼 retry 루프** |
| `cookies()` | cookie jar 활성화 | off | **래퍼 cookie jar** |
| `proxy(url)` / `proxyBasicAuth(u, p)` | HTTP proxy | off | undici `ProxyAgent` |
| `compression()` | gzip/deflate 투명 해제 | off | **래퍼 해제** |

## 네이티브 위임 vs 래퍼 구현

undici `request`는 auto-redirect·auto-decompress·cookie를 하지 않는다. 그래서 래퍼가
redirect 루프·cookie jar·압축 해제를 직접 수행해 의미론을 4언어에서 동일하게 맞춘다.
connection pool·proxy·TLS는 undici dispatcher에 위임한다.

## client 재사용

client는 내부 dispatcher 하나를 감싸며 connection pool을 공유한다. **한 번 만들어
재사용**하고 끝나면 `close()`로 정리한다.

## 요청별 timeout override

```ts
await client.get('/slow-report').timeout(30000).submitRaw();
```

[다음: Request 만들기 →](04-making-requests.ko.md)
