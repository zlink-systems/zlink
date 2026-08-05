# 2. Client builder 계약

> [공통 계약 목차](README.ko.md)

## 2.1 진입점

- 정적 팩토리 `create()` / `create(baseUrl)` → client builder → `build()` → client.
- kotlin은 언어 편차로 top-level DSL `zlinkHttpClient(baseUrl) { ... }`를
  제공한다(내부적으로 동일 builder).
- client builder에도 verb 단축(`get(path)` 등 7종)이 있다. 이 경로는
  **one-shot**이다: 제출 시점에 client를 lazy build하고 완료 후 닫는다.
  connection pool 재사용이 없으므로 반복 호출용이 아니다([5장](05-execution-model.ko.md) §5.4).

## 2.2 옵션과 기본값 표 (정본)

모든 언어는 아래 옵션 전부를 제공하며, 기재된 기본값은 5개 언어 공통 계약이다.
이름 케이싱은 언어 관용을 따른다([언어별 인터페이스 정의](language-interfaces.ko.md)).

| 옵션 (공통 개념명) | 인자 | 기본값 | 의미 |
| --- | --- | --- | --- |
| `baseUrl` | URL 문자열 | 없음(필수: create 인자 또는 옵션으로 지정) | 모든 요청 path의 기준 URL |
| `timeout` | 시간 | **3000ms** | 시도(attempt)당 timeout. 요청별 override 가능 |
| `defaultHeader` | name, value | 없음(누적) | 모든 요청에 붙는 기본 헤더. 요청별 헤더가 이긴다 |
| `basicAuth` | user, password | off | `Authorization: Basic` |
| `bearerToken` | token | off | `Authorization: Bearer` |
| `maxResponseBodySize` | bytes | **16 MiB** | 응답 body 상한. 압축 해제 후 크기에도 적용. 초과 시 `CapacityExceeded` |
| `trustCertificateFile` | PEM 경로 | 시스템 root | 신뢰 인증서 **추가**(기본 root 대체 아님) |
| `clientCertificateFile` | cert 경로, key 경로 | off | mTLS 클라이언트 인증서(PEM) |
| `followRedirects` | max (무인자 시 **5**) | **off** | redirect 자동 추적 한도([6장](06-redirect-retry-cookie.ko.md)) |
| `retry` | attempts | **off(0)** | 추가 재시도 횟수. 총 시도 = 1 + attempts |
| `cookies` | 없음 | **off** | cookie jar 활성화 |
| `proxy` | URL (`http://`만) | off | HTTP proxy([7장](07-auth-tls-proxy.ko.md)) |
| `proxyBasicAuth` | user, password | off | proxy 인증 |
| `compression` | 없음 | **off** | `Accept-Encoding: gzip, deflate` + 투명 해제([8장](08-compression.ko.md)) |

검증 규칙: `timeout`/`maxResponseBodySize`/`followRedirects`/`retry`는 양수,
`proxy`는 `http://` prefix, `baseUrl`은 `http://` 또는 `https://`로 시작해야
하며 미지정 상태의 `build()`는 `ProtocolError`. 검증 실패는 builder
호출 시점에 즉시 던진다(eager).

## 2.3 언어 편차 (인정된 것)

| 언어 | 편차 |
| --- | --- |
| cpp | `coroutines()` / `coroutines(resume)` / `coroutines(execute, resume)` — 실행 모델 스위치([5장](05-execution-model.ko.md)). 다른 언어는 런타임이 항상 비동기라 해당 개념 없음 |
| dotnet | `Codecs(Action<IZLinkCodecRegistryBuilder>)` — framework codec extension 등록. 타 언어는 JSON 고정(§10 R-항목 아님, dotnet만 spec으로 고정된 확장점) |
| node | `timeout`이 정수 ms (다른 언어는 시간 타입: `std::chrono`, `TimeSpan`, `Duration`) |
| kotlin | builder 대신 DSL 블록. `build()` 호출 없음 |

## 2.4 client 수명 규칙

- client는 **서비스당 하나 만들어 재사용**한다. 요청마다 create/build 하면
  connection pool과 keep-alive 이득을 잃는다.
- client는 명시적으로 닫는다: `close()` / `Dispose()` / `AutoCloseable` /
  언어 관용 리소스 구문(`use`, try-with-resources).
