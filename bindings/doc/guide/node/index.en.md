---
title: "Node.js Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: Java](../java/index.en.md) | [Next: Python](../python/index.en.md)
<!-- bindings-nav:end -->

# Node.js Binding Guide (`@zlink-systems/zlink`)

> **Contract-owning document for this chapter** — the [Node.js bindings spec](../../spec/node/README.en.md)
> covers it. This chapter shows that contract as working sample code.

Explains how to use zlink in Node.js through working sample code.
See the [core guide](https://kairos-code-dev.github.io/zlink/guide/01-overview/) for
messaging concepts.

---

## Installation

```bash
npm install @zlink-systems/zlink
```

- **Node.js 22** or later.
- The native core is bundled as a per-platform prebuild.

```javascript
const zlink = require('@zlink-systems/zlink');
// or ESM / TypeScript
import * as zlink from '@zlink-systems/zlink';
```

---

## 5-Minute Example

```javascript
const zlink = require('@zlink-systems/zlink');

// Server
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
// Client
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

## Core Types

### Context

```javascript
const ctx = zlink.createContext();
// always close after use — this interrupts blocking operations on child sockets
ctx.close();
```

### Message

The Node binding uses `Buffer` directly as a message. `message()` makes a copy,
so you're free to reuse the original Buffer.

```javascript
socket.send().message(Buffer.from('hello')).submit();
socket.send().message(Buffer.from([0x01, 0x02])).submit();

// access the payload after receiving
const received = new zlink.Received();
socket.recv(received);
const data = received.parts[0].data();   // Buffer
const text = data.toString('utf8');
received.close();
```

### Received — the receive envelope

```javascript
const received = new zlink.Received();
socket.recv(received);                  // synchronous, blocking
try {
  const parts = received.parts;         // Message[]
  const rid = received.routingId;       // RoutingId or null
  const seq = received.requestSeq;      // bigint or null
} finally {
  received.close();
}
```

### Routing ID

```javascript
const rid = zlink.RoutingId.from(Buffer.from('server-01'));
socket.setRoutingId(rid);
```

---

## Ownership And Lifetime

| Situation | Rule |
|------|------|
| `submit()` succeeds | the passed Buffer is copied internally, so you can reuse the original |
| `recv()` succeeds | `received.close()` is required (prefer a finally block) |
| `submit()` (Promise) completes | close each reply part with `part.close()` |
| `ctx.close()` | interrupts blocking operations on child sockets |

```javascript
const received = new zlink.Received();
socket.recv(received);
try {
  // process parts
} finally {
  received.close();
}
```

---

## Error Handling

The Node binding throws per-operation error classes.

```javascript
try {
  socket.send().message(Buffer.from('data')).submit();
} catch (error) {
  if (error instanceof zlink.SubmitError) {
    if (error.result === zlink.SubmitResult.Backpressured) {
      // retry
    } else {
      throw error;
    }
  }
}
```

Error classes: `SubmitError`, `RequestError`, `RecvError`, `BindError`,
`ConnectError`, `ConfigError`, `CloseError`, `HandlerError`.
Each exposes the result code via a `.result` property.

---

## C API Mapping

| C API | Node API |
|-------|----------|
| `zlink_ctx_new()` | `zlink.createContext()` |
| `zlink_ctx_term()` | `ctx.close()` |
| `zlink_socket(ctx, type)` | `zlink.createPairSocket(ctx)`, etc. |
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

## Native Library / Deployment

The native core ships in the package as a per-platform prebuild. Works with
just `npm install`, no separate build step.

```javascript
const [major, minor, patch] = zlink.version(); // [number, number, number]
console.log(`zlink ${major}.${minor}.${patch}`);
```

**Threading notes.** Node uses a single-threaded event-loop model.

| Item | Rule |
|------|------|
| `Context` / sockets | used on the main event loop |
| Blocking `recv()` | blocks the event loop, so keep it short or prefer non-blocking + poller |
| Async `submit()` | Promise-based — doesn't block the event loop |

---

## Samples

Verified samples live under `bindings/node/samples/`.

| File | Description |
|------|------|
| `pair_recv_sample.ts` | PAIR send/receive |
| `dealer_router_recv_sample.ts` | DEALER/ROUTER send/receive |
| `request_reply_sample.ts` | Request/reply |
| `pubsub_recv_sample.ts` | PUB/SUB publish/subscribe |
| `stream_recv_sample.ts` | STREAM raw TCP |
| `stream_packet_callback_sample.ts` | STREAM packet callback |
| `monitor_recv_sample.ts` | Monitor event receive |

> SPOT/Actor examples are covered by the framework samples, not the core
> binding — see the service links under [See Also](#see-also) below.

```bash
cd bindings/node
npm run build
node dist-tools/samples/pair_recv_sample.js
```

---

## JavaScript

JavaScript uses the Node binding (`@zlink-systems/zlink`) **as-is, with no
separate native binding**. The installation, core types, ownership, errors, and
mapping table above all apply identically, minus the TypeScript type
annotations.

- **Dependency**: `@zlink-systems/zlink` (same as above). No TypeScript build
  step needed — `require` it directly as plain `.js`.

```javascript
const zlink = require('@zlink-systems/zlink');

const ctx = zlink.createContext();
const socket = zlink.createPairSocket(ctx);
// ... after use: socket.close(); ctx.close();
```

- **Ownership**: clean up with explicit `close()` calls (same as Node — doesn't
  rely on GC).
- **Samples**: `bindings/javascript/samples/` (`.js`) has the same canonical
  set as the Node samples. Build the Node binding, then run directly with
  `node`.

```bash
cd bindings/node && npm run build      # build the shared runtime
cd ../javascript/samples
node pair_recv_sample.js                # or ./run_samples.sh
```

The core guide's language tabs have a dedicated **JavaScript** column, so you
can see messaging/service usage directly in JavaScript code.

---

## See Also

**Socket patterns**
- [Socket pattern overview](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  — [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/) · [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/) · [Proxy](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**Services**
- [Framework service overview](../../../../framework/doc/framework/common/guide/server/03-concepts.en.md)

**Operations**
- [Socket options](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/)
- [TLS security](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/)
- [Monitoring](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/)
- [Thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)
- [Message API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/)
- [Routing ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
