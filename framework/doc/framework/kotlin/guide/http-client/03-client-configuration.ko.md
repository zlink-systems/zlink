[← 목차](README.ko.md)

# 3. Client 구성

`zlinkHttpClient(baseUrl) { ... }`의 DSL 블록은 client 전역 설정을 모은다. 블록 안에서는
fluent builder 메서드를 그대로 호출한다.

## builder 옵션

기본값은 [공통 spec 2장](../../../common/spec/http-client/02-client-builder.ko.md)이 정본이다.

| 옵션 | 효과 | 기본값 |
| --- | --- | --- |
| `timeout(Duration)` | 시도당 timeout(요청별 override 가능) | **3000ms** |
| `defaultHeader(name, value)` | 모든 요청에 붙는 기본 헤더 | 없음 |
| `basicAuth(user, pass)` / `bearerToken(token)` | `Authorization` 헤더 | off |
| `maxResponseBodySize(bytes)` | 응답 본문 상한(decoded 기준) | **16 MiB** |
| `trustCertificateFile(path)` | 신뢰 인증서 추가 | 시스템 root |
| `clientCertificateFile(cert, key)` | mTLS client 인증서 | off |
| `followRedirects(max)` | redirect 추적(무인자 시 **5회**) | off |
| `retry(attempts)` | transport 실패 재시도(총 1+n회 시도) | off |
| `cookies()` | cookie jar 활성화 | off |
| `proxy(url)` / `proxyBasicAuth(user, pass)` | HTTP proxy | off |
| `compression()` | gzip/deflate 투명 해제 | off |

```kotlin
val client = zlinkHttpClient("https://game-api.example.internal") {
    timeout(Duration.ofSeconds(5))
    bearerToken("eyJhbGci...")
    compression()
}
```

## client 재사용

client는 connection pool 하나를 공유한다. **한 번 만들어 재사용**하고 `use { ... }`나
`close()`로 정리한다. 장기 client는 애플리케이션 수명과 함께 두고 요청마다 새로 만들지
않는다.

```kotlin
class GameApi(baseUrl: String) : AutoCloseable {
    private val client = zlinkHttpClient(baseUrl)
    suspend fun profile(id: Long) = client.get("/players/$id").fetch<PlayerProfile>()
    override fun close() = client.close()
}
```

## 요청별 timeout override

```kotlin
client.get("/slow-report").timeout(Duration.ofSeconds(30)).awaitRaw()
```

[다음: Request 만들기 →](04-making-requests.ko.md)
