# Node.js Quickstart — 빈 프로젝트에서 첫 요청까지

> **이 장의 계약 소유 문서** — 없다. API의 정식 계약은
> [Node.js 스펙](../common/spec/server/languages/node/README.ko.md)이 다룬다.

저장소의 [`framework/languages/node/quickstart/`](../../../languages/node/quickstart/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다.

location store 없이 process 둘이 서로의 endpoint를 직접 지정해 request/reply 한 번을
주고받는다. 다음 단계는 [설치와 첫 동작](guide/server/02-getting-started.ko.md)이다.

## 전제

- Node.js 22 이상 (`@zlink-systems/zlink`의 `engines`)
- npm 접근

## 1. 패키지 버전

`@zlink-systems/zlink`(binding)는 적지 않는다. `@zlink-systems/framework`가 의존 버전을
선언한다.

```json title="package.json"
--8<-- "framework/languages/node/quickstart/package.json"
```

## 2. 공유 계약

요청 타입은 class여야 한다. 패킷 이름을 `payload.constructor.name`에서 얻으므로 객체
리터럴은 쓸 수 없다. 응답 타입은 이름으로 조회하지 않아 `interface`로 둔다.

```typescript title="Shared/contracts.ts"
--8<-- "framework/languages/node/quickstart/Shared/contracts.ts"
```

## 3. 처리하는 쪽

handler는 두 곳에 등록한다 — Nest의 `providers`와 `channel(...).server()`의
`addRequestHandler`. `0.0.0.0`으로 bind하고 `setAdvertiseHost`를 생략하면 `127.0.0.1`을 광고한다.
다른 host에서 접속해야 하면 `setAdvertiseHost`로 접속 가능한 주소를 지정한다.

```typescript title="Server/main.ts"
--8<-- "framework/languages/node/quickstart/Server/main.ts"
```

## 4. 호출하는 쪽

```typescript title="Client/main.ts"
--8<-- "framework/languages/node/quickstart/Client/main.ts"
```

## 5. 실행

```bash
cd framework/languages/node/quickstart
npm install
npm run build

# 터미널 두 개. server를 먼저 실행한다.
npm run server
npm run client

curl http://127.0.0.1:5080/hello/world
```

응답은 `"hello, world"`, 상태 코드 200이다.

## 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `package.json` | `@zlink-systems/framework`·`@zlink-systems/nestjs` 두 항목. binding은 전이 의존으로 둔다 |
| `Shared/contracts.ts` | 요청은 class, 응답은 `interface` |
| `Server/main.ts` | `addRouteMesh` → `listen` → `setAdvertiseHost` → `channel(...).server().addRequestHandler(...)`, handler를 `providers`에 등록 |
| `Client/main.ts` | `channel(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...).submit<T>()` |

수동 `peerConnections().connect` 대신 location store를 쓰는 구성은
[10. Location](guide/server/10-location.ko.md)이 다룬다.
