# Spec -- ZLink HTTP Client For Node

> 사용법 중심 문서는 [사용자 가이드](../../../../../node/guide/http-client/README.ko.md)를 본다.
> **언어 중립 공통 계약은 [공통 spec](../../README.ko.md)이 정본**이며,
> 이 문서는 공통 계약에 대한 Node 고유 편차와 구현 매핑만 기술한다.
> 실제 계약의 단일 기준은 공통 spec + `packages/http-client/src/**` 공개 타입과
> `test/contract/http-client.test.js` 회귀 테스트다.

## 1. 목적

`@zlink-systems/http-client`는 Node에서 HTTP request를 보내기 위한 별도 client-side
산출물이다. JSON 전용 client가 아니라 일반 HTTP client이며 zlink fluent builder 스타일로
undici의 낮은 수준 설정을 흡수한다. typed JSON 경로(`body(dto)`/`async<T>()`)는 그 위에
얹은 편의 계층이다.

`@zlink-systems/framework`의 에러 모델(`ZLinkFrameworkException`)에 의존하지만 framework
core의 기본 의존성은 아니다(단방향 의존).

## 2. 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| 라이브러리 contract | `packages/http-client/src/{index,client,request-builder,types}.ts` | package 내부 공개 표면 |
| runtime 구현 | `packages/http-client/src/runtime/*` | internal |
| 회귀 테스트 | `test/contract/http-client.test.js` | private |
| 패키지 | `@zlink-systems/http-client` | 현재 workspace 전용 private package |

공개 표면에는 undici `Dispatcher`/`Agent`/`request` 타입을 노출하지 않는다.

## 3. 공개 타입

- `ZLinkHttpClient` — `create()` / `create(baseUrl)`, 메서드 `get/post/put/delete/
  patch/head/options`, `close()`.
- `ZLinkHttpClientBuilder` — `baseUrl`, `timeout`, `defaultHeader`, `basicAuth`,
  `bearerToken`, `maxResponseBodySize`, `trustCertificateFile`, `clientCertificateFile`,
  `followRedirects`, `retry`, `cookies`, `proxy`, `proxyBasicAuth`, `compression`,
  `build`, 그리고 단발 verb shortcut.
- `ZLinkHttpRequestBuilder` — `header`, `query`, `timeout`, `body`(JSON/raw 오버로드),
  `bodyStream`, `form`, `multipart`, `multipartFile`, `submitRaw`, `download`, `async<T>`,
  `fetch<T>`(`Promise<T>`, body만 반환).
- `ZLinkHttpServerRequestBuilder` — standalone 표면과 one-way
  `submit(): Promise<void>`를 제공한다. One-way 완료에는 전송 결과나 admission status가 없다.
  Node HTTP Client의 typed response terminal은 `async<T>(): Promise<HttpResponse<T>>`를 유지한다.
  TypeScript의 generic type은 runtime에서 제거되므로 no-argument `submit<T>()`와 one-way
  `submit()`을 같은 상속 계층에 선언하면 두 operation을 구분할 수 없기 때문이다.
- `RawHttpResponse` { `status`, `headers`, `body` }.
- `HttpResponse<T>` { `status`, `headers`, `body`, `rawBody` }.
- `ZLinkHttpMethod`, `BodyChunkProvider`(`() => Uint8Array | null`), `DownloadSink`.

## 4. 실행 모델

- 모든 제출은 `Promise`를 돌려준다. undici의 libuv 비동기 I/O로 네트워크 대기 중 event
  loop는 점유되지 않는다.
- 단일 event loop인 Node에는 continuation 재개 위치 주입이 없다(`.coroutines()` 없음).
- Node에는 동기 blocking HTTP 접근이 없다.

## 5. 전송 의미론

기본값·redirect·retry·cookie·압축·인증 스크럽·body 소스 배타 의미론은
[공통 spec 2~8장](../../README.ko.md)을 따른다. Node 구현 매핑:

- **백엔드**: undici 저수준 `request`(`fetch` 아님 — auto-redirect/decompress/
  cookie가 없어 의미론을 래퍼가 통제).
- **TLS**: `trustCertificateFile`→`Agent.connect.ca`(기본 root에 추가),
  mTLS→`connect.cert/key`.
- **proxy**: `ProxyAgent` — 인증은 헤더가 아니라 `token`으로 전달되어 CONNECT
  tunnel에서 target에 노출되지 않는다.
- **압축 해제**: `node:zlib`.
- typed JSON 디코드에 prototype-pollution 방어(`__proto__`/`constructor`/
  `prototype` 제거) — Node 고유 보안 규칙.

## 6. 에러 매핑

[공통 spec 9장](../../09-error-model.ko.md)을 따른다. Node는 Framework 공통 kind만 사용한다.

- timeout은 `DeadlineExceeded`로 보고하고, 예외의 `cause`는
  `Error`이며 `name`을 정확히 `TimeoutError`로 고정한다.

## 7. 회귀 테스트 / 등록

- 회귀 테스트: `test/contract/http-client.test.js`(node:test). chunked 업로드·retry는
  raw `net` 서버, TLS/mTLS는 `node:https` + `test/fixtures/tls/` 인증서로 검증.
- 등록: workspace `package.json`(undici 런타임 의존), 루트 `package-lock.json`,
  `tsconfig.base.json` paths, `tsconfig.build.json` references, ESLint flat config(자동 scope).
- 커버리지: node 내장 coverage 게이트(`packages/*/dist`) 기준 80% 초과.
