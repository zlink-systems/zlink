# Node.js Quickstart — 설치부터 첫 요청까지

!!! info "이 장을 읽고 나면"

    패키지를 설치하고, 두 process가 서로 호출하는 최소 project를 실행할 수 있다.

저장소의 [`framework/languages/node/quickstart/`](../../../languages/node/quickstart/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다. location store 없이
process 둘이 서로의 endpoint를 직접 지정해 request/reply 한 번을 주고받는다.

## 0. 예제 저장소 clone

이 장의 project는 `zlink-node-examples` 저장소의 `quickstart/`이며, 기능 가이드가 읽는
program인 `tutorial/`과 `samples/`도 같은 저장소에 있다.

```bash
git clone https://github.com/zlink-systems/zlink-node-examples.git
cd zlink-node-examples/quickstart
```

`main`은 최신 릴리스에 그 뒤의 수정을 더한 것이고, 패키지 버전은 그 릴리스에 맞춰져 있다.
이전 릴리스는 tag `vA.B.C`로 받는다(`git checkout vA.B.C`). 이슈와 PR은 `zlink-systems/zlink`로
보낸다.

## 1. 설치

- Node.js 22 이상 (`@zlink-systems/zlink`의 `engines`)
- npm 접근

npm에서 받는다. 서버 하나를 만들 때 필요한 최소 조합은 다음과 같다.

```bash
npm install @zlink-systems/framework   # 계약과 runtime
npm install @zlink-systems/nestjs      # DI·모듈 등록
```

**`@zlink-systems/zlink`(binding)는 적지 않는다.** `@zlink-systems/framework`가 의존 버전을
선언한다.

```json title="package.json"
--8<-- "framework/languages/node/quickstart/package.json"
```

필요할 때 추가하는 package는 다음과 같다.

| package | 언제 추가하나 |
| --- | --- |
| `@zlink-systems/framework-locations-redis` | Redis location store로 자동 연결을 사용할 때([Location](guide/server/25-location.ko.md)) |
| `@zlink-systems/framework-codec-protobuf` · `-codec-msgpack` | 기본 JSON codec 대신 사용할 때([Handler와 메시지 처리](guide/server/31-handler-dispatch.ko.md#3-codec--payload를-바이트로-바꾼다)) |
| `@zlink-systems/stream-connector` | 외부 client(게임 client·모바일)를 만들 때([STREAM](guide/server/23-stream.ko.md)) |
| `@zlink-systems/http-client` | 서버에서 HTTP를 호출할 때([HTTP Client 가이드](guide/http-client/README.ko.md)) |

라이선스는 계층마다 다르다 — core·binding은 MPL-2.0, framework는 FSL-1.1-ALv2,
`@zlink-systems/http-client`는 Apache-2.0이다. 서비스를 만들어 파는 데 드는 비용은 없다.

## 2. 공유 계약

요청 타입은 class여야 한다. 패킷 이름을 `payload.constructor.name`에서 얻으므로 객체
리터럴은 사용할 수 없다. 응답 타입은 이름으로 조회하지 않아 `interface`로 둔다.

```typescript title="Shared/contracts.ts"
--8<-- "framework/languages/node/quickstart/Shared/contracts.ts"
```

## 3. 처리하는 쪽

handler는 두 곳에 등록한다 — Nest의 `providers`와 `channel(...).server()`의
`addRequestHandler`. 이 예제는 `127.0.0.1`에 bind하므로 같은 host의 process만 접속하며, Windows에서
방화벽 허용 창이 뜨지 않는다. 다른 host에서 접속해야 하면 접속 가능한 주소에 bind하고
`setAdvertiseHost`로 그 주소를 광고한다.

```typescript title="Server/main.ts"
--8<-- "framework/languages/node/quickstart/Server/main.ts"
```

## 4. 호출하는 쪽

```typescript title="Client/main.ts"
--8<-- "framework/languages/node/quickstart/Client/main.ts"
```

## 5. 실행

```bash
cd zlink-node-examples/quickstart
npm install
npm run build

# 터미널 두 개. server를 먼저 실행한다.
npm run server
npm run client

curl http://127.0.0.1:5080/hello/world
```

응답은 `"hello, world"`, 상태 코드 200이다.

## 6. 첫 실행이 안 될 때 확인할 항목

| 증상 | 확인할 항목 |
| --- | --- |
| package를 찾지 못한다 | §1의 package 이름을 그대로 적었는지 확인한다. binding 버전은 따로 고정하지 않는다 |
| startup이 실패한다 | 두 process의 mesh 이름이 같은지, listen endpoint가 다른 process와 겹치지 않는지 확인한다 |
| 패킷 이름이 맞지 않는다 | 요청 타입을 class로 선언했는지 확인한다. 객체 리터럴은 이름을 얻지 못한다 |
| 호출이 대상 없음으로 끝난다 | 받는 쪽이 그 channel 이름을 server 역할로 등록했는지, handler를 `providers`에도 등록했는지 확인한다 |
| 응답이 오지 않는다 | 보낸 쪽이 `request`를 썼는지 확인한다. `send`는 응답을 받지 않는다 |

## 7. 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `package.json` | `@zlink-systems/framework`·`@zlink-systems/nestjs` 두 항목. binding은 전이 의존으로 둔다 |
| `Shared/contracts.ts` | 요청은 class, 응답은 `interface` |
| `Server/main.ts` | `addRouteMesh` → `listen` → `setAdvertiseHost` → `channel(...).server().addRequestHandler(...)`, handler를 `providers`에 등록 |
| `Client/main.ts` | `channel(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...).submit<T>()` |

## 8. 다음으로 읽을 것

이 두 process는 endpoint를 서로 직접 적어 연결한다. 서버를 늘리거나 다른 주소로 다시 시작해도
호출 코드를 그대로 두려면 자동 연결이 필요하고, 그것은
[Location](guide/server/25-location.ko.md)이 다룬다.

- 개념을 먼저 확인할 때 — [핵심 개념](guide/server/03-concepts.ko.md)
- 이름으로 호출하는 경로 — [Channel 메시징](guide/server/20-channel-messaging.ko.md)
- id로 호출하는 상태 객체 — [Spot](guide/server/21-spot.ko.md) · [Actor](guide/server/22-actor.ko.md)
- 완결된 업무 흐름을 볼 때 — [샘플 고르기](guide/server/14-samples.ko.md)
