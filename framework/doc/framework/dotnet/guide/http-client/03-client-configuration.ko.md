[← 목차](README.ko.md)

# 3. Client 구성

Builder는 client에서 공통으로 사용할 URL, timeout, 인증과 전송 정책을 모은다.

## builder 옵션

기본값은 [공통 spec 2장](../../../common/spec/http-client/02-client-builder.ko.md)이 정본이다.

| 옵션 | 효과 | 기본값 |
| --- | --- | --- |
| `BaseUrl(url)` | 모든 요청의 기준 URL | 없음(필수) |
| `Timeout(span)` | 시도당 timeout(요청별 override 가능) | **3000ms** |
| `DefaultHeader(n, v)` | 모든 요청에 붙는 기본 헤더 | 없음 |
| `BasicAuth(u, p)` / `BearerToken(t)` | `Authorization` 헤더 | off |
| `MaxResponseBodySize(bytes)` | 응답 본문 상한(decoded 기준) | **16 MiB** |
| `TrustCertificateFile(path)` | 신뢰 인증서 추가 | 시스템 root |
| `ClientCertificateFile(cert, key)` | mTLS client 인증서 | off |
| `FollowRedirects(max)` | redirect 추적(무인자 시 **5회**) | off |
| `Retry(attempts)` | transport 실패 재시도(총 1+n회 시도) | off |
| `Cookies()` | cookie jar 활성화 | off |
| `Proxy(url)` / `ProxyBasicAuth(u, p)` | HTTP proxy와 인증 | off |
| `Compression()` | gzip/deflate 투명 해제 | off |
| `Codecs(configure)` | `.NET` codec extension 등록 | JSON |

## framework 서버에 등록

Spot handler에서 사용하는 client는 이름을 붙여 DI에 등록한다. 서버 등록은 connection pool을
재사용하고 callback 완료를 현재 Spot 실행 줄의 새 turn에 연결한다.

```csharp
services.AddZLinkHttpClient("player-api", http => http
    .BaseUrl("https://player-api.internal") // 이 이름으로 주입되는 client의 기준 URL을 고정한다.
    .Timeout(TimeSpan.FromSeconds(3)));      // 외부 API의 시도당 timeout을 한곳에서 관리한다.
```

handler는 같은 이름의 `ZLinkHttpServerClient`를 주입받는다. 정적 팩토리로 만든
`ZLinkHttpClient`는 client-side 코드용이므로 one-way `Async()`를 제공하지 않는다.
두 client 모두 HTTP request builder에 `Yield`를 제공하지 않는다.

## client 재사용과 connection pool

요청마다 client를 새로 만들지 말고 **한 번 만들어 재사용**한다. 단발 한 줄 요청(§2)은
반복 호출에 사용하지 않는다.

## 요청별 timeout override

```csharp
await client.Get("/slow-report").Timeout(TimeSpan.FromSeconds(30)).AsyncRaw();
```

[다음: Request 만들기 →](04-making-requests.ko.md)
