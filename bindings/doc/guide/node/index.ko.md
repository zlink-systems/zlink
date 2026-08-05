---
title: "Node.js 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: Java](../java/index.ko.md) | [다음: Python](../python/index.ko.md)
<!-- bindings-nav:end -->

# Node.js 바인딩 가이드 (`@zlink-systems/zlink`)

> **이 장의 계약 소유 문서** — [Node.js bindings 스펙](../../spec/node/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

Node.js에서 zlink를 쓰는 방법을 실제 샘플 코드 중심으로 설명합니다.
메시징 개념은 [코어 가이드](https://kairos-code-dev.github.io/zlink/guide/01-overview/)를 참고하세요.

---

## 설치

```bash
npm install @zlink-systems/zlink
```

- **Node.js 22** 이상.
- 네이티브 코어가 플랫폼별 prebuild로 번들됩니다.

```javascript
const zlink = require('@zlink-systems/zlink');
// 또는 ESM / TypeScript
import * as zlink from '@zlink-systems/zlink';
```

---

## 5분 예제

```javascript
const zlink = require('@zlink-systems/zlink');

// 서버
const ctx = zlink.createContext();
const server = zlink.createPairSocket(ctx);
server.bind('tcp://127.0.0.1:5555');

const received = new zlink.Received();
server.recv(received);
console.log(received.parts[0].data().toString()); // PING
received.close();

server.send().message(Buffer.from('ACK')).submit();

server.close();
ctx.close();
```

```javascript
// 클라이언트
const ctx = zlink.createContext();
const client = zlink.createPairSocket(ctx);
client.connect('tcp://127.0.0.1:5555');

client.send().message(Buffer.from('PING')).submit();

const received = new zlink.Received();
client.recv(received);
console.log(received.parts[0].data().toString()); // ACK
received.close();

client.close();
ctx.close();
```

---

## 핵심 타입

### 컨텍스트

```javascript
const ctx = zlink.createContext();
// 사용 후 반드시 close — 하위 소켓의 블로킹 작업이 중단됩니다
ctx.close();
```

### 메시지

Node 바인딩은 `Buffer`를 메시지로 직접 씁니다. `message()`를 호출하면 복사본을
만들기 때문에 원본 Buffer를 마음껏 재사용할 수 있습니다.

```javascript
socket.send().message(Buffer.from('hello')).submit();
socket.send().message(Buffer.from([0x01, 0x02])).submit();

// 수신 후 페이로드 접근
const received = new zlink.Received();
socket.recv(received);
const data = received.parts[0].data();   // Buffer
const text = data.toString('utf8');
received.close();
```

### Received — 수신 봉투

```javascript
const received = new zlink.Received();
socket.recv(received);                  // 동기 블로킹
try {
  const parts = received.parts;         // Message[]
  const rid = received.routingId;       // RoutingId 또는 null
  const seq = received.requestSeq;      // bigint 또는 null
} finally {
  received.close();
}
```

### 라우팅 ID

```javascript
const rid = zlink.RoutingId.from(Buffer.from('server-01'));
socket.setRoutingId(rid);
```

---

## 소유권과 수명

| 상황 | 규칙 |
|------|------|
| `submit()` 성공 | 전달된 Buffer는 내부 복사되므로 원본 재사용 가능 |
| `recv()` 성공 | `received.close()` 필수 (finally 블록 권장) |
| `submit()`(Promise) 완료 | 회신 파트 배열을 각각 `part.close()` |
| `ctx.close()` | 하위 소켓의 블로킹 작업 중단 |

```javascript
const received = new zlink.Received();
socket.recv(received);
try {
  // 파트 처리
} finally {
  received.close();
}
```

---

## 에러 처리

Node 바인딩은 작업별 에러 클래스를 던집니다.

```javascript
try {
  socket.send().message(Buffer.from('data')).submit();
} catch (error) {
  if (error instanceof zlink.SubmitError) {
    if (error.result === zlink.SubmitResult.Backpressured) {
      // 재시도
    } else {
      throw error;
    }
  }
}
```

에러 클래스는 `SubmitError`, `RequestError`, `RecvError`, `BindError`,
`ConnectError`, `ConfigError`, `CloseError`, `HandlerError`입니다.
각각 `.result` 속성으로 결과 코드를 노출합니다.

---

## C API 대응표

| C API | Node API |
|-------|----------|
| `zlink_ctx_new()` | `zlink.createContext()` |
| `zlink_ctx_term()` | `ctx.close()` |
| `zlink_socket(ctx, type)` | `zlink.createPairSocket(ctx)` 등 |
| `zlink_bind(s, ep)` | `socket.bind(ep)` |
| `zlink_connect(s, ep)` | `socket.connect(ep)` |
| `zlink_send_part(...)` | `socket.send().message(buf).submit()` |
| `zlink_recv_part(...)` | `socket.recv(received)` |
| `zlink_msg_data(msg)` | `part.data()` (Buffer) |
| `zlink_routing_id_t` | `zlink.RoutingId` |
| `zlink_socket_monitor_open(...)` | `socket.monitorOpen([...])` |
| `zlink_poller_new()` | `zlink.createPoller()` |
| `zlink_timer_new()` | `zlink.createTimer()` |

---

## 네이티브 라이브러리 / 배포

네이티브 코어는 플랫폼별 prebuild로 패키지에 들어갑니다. 별도 빌드 없이
`npm install`만으로 동작합니다.

```javascript
const [major, minor, patch] = zlink.version(); // [number, number, number]
console.log(`zlink ${major}.${minor}.${patch}`);
```

**스레딩 유의사항.** Node는 단일 스레드 이벤트 루프 모델입니다.

| 항목 | 규칙 |
|------|------|
| `Context`·소켓 | 메인 이벤트 루프에서 사용 |
| 블로킹 `recv()` | 이벤트 루프를 막으므로 짧게 사용하거나 논블로킹 + 폴러 권장 |
| 비동기 `submit()` | Promise 기반 — 이벤트 루프를 막지 않음 |

---

## 샘플

`bindings/node/samples/` 디렉터리의 검증된 샘플입니다.

| 파일 | 설명 |
|------|------|
| `pair_recv_sample.ts` | PAIR 송수신 |
| `dealer_router_recv_sample.ts` | DEALER/ROUTER 송수신 |
| `request_reply_sample.ts` | 요청/응답 |
| `pubsub_recv_sample.ts` | PUB/SUB 발행·구독 |
| `stream_recv_sample.ts` | STREAM 원시 TCP |
| `stream_packet_callback_sample.ts` | STREAM 패킷 콜백 |
| `monitor_recv_sample.ts` | 모니터 이벤트 수신 |

> SPOT·Actor 예제는 core 바인딩이 아니라 framework 샘플이 다룬다 — 아래
> [더 보기](#더-보기)의 서비스 링크를 본다.

```bash
cd bindings/node
npm run build
node dist-tools/samples/pair_recv_sample.js
```

---

## JavaScript

JavaScript는 **별도 네이티브 바인딩 없이 Node 바인딩(`@zlink-systems/zlink`)을
그대로** 씁니다. 위의 설치·핵심 타입·소유권·에러·대응표가 똑같이 적용되고
TypeScript 타입 표기만 빠집니다.

- **의존성**: `@zlink-systems/zlink`(위와 동일). TypeScript 빌드 단계가 필요 없고
  순수 `.js`로 바로 `require`한다.

```javascript
const zlink = require('@zlink-systems/zlink');

const ctx = zlink.createContext();
const socket = zlink.createPairSocket(ctx);
// ... 사용 후 socket.close(); ctx.close();
```

- **소유권**: 명시적 `close()`로 정리한다(Node와 동일, GC에 의존하지 않는다).
- **샘플**: `bindings/javascript/samples/`(`.js`)에 Node 샘플과 같은 canonical
  세트가 있다. Node 바인딩을 빌드한 뒤 `node`로 바로 실행한다.

```bash
cd bindings/node && npm run build      # 공유 런타임 빌드
cd ../javascript/samples
node pair_recv_sample.js                # 또는 ./run_samples.sh
```

코어 가이드의 언어 탭에는 **JavaScript** 칸이 따로 있어 메시징·서비스 사용법을
JavaScript 코드로 바로 볼 수 있다.

---

## 더 보기

**소켓 패턴**
- [소켓 패턴 개요](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  — [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/) · [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/) · [프록시](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**서비스**
- [Framework 서비스 개요](../../../../framework/doc/framework/common/guide/server/03-concepts.ko.md)

**운영**
- [소켓 옵션](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/)
- [TLS 보안](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/)
- [모니터링](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/)
- [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)
- [메시지 API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/)
- [라우팅 ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
