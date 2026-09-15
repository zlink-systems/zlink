# Node.js Quickstart — from an empty project to a first request

> **Contract owner for this chapter** — none. The formal API contract is in the
> [Node.js spec](../common/spec/server/languages/node/README.en.md).

The project lives at
[`framework/languages/node/quickstart/`](../../../languages/node/quickstart/). The code
blocks below are read from those files when the site is built.

Without a location store, two processes name each other's endpoint directly and exchange one
request/reply. The next step is
[Installation and first run](guide/server/02-getting-started.en.md).

## Prerequisites

- Node.js 22 or later (`engines` of `@zlink-systems/zlink`)
- Access to npm

## 1. Package versions

`@zlink-systems/zlink` (the binding) is not listed. `@zlink-systems/framework` declares the
version it depends on.

```json title="package.json"
--8<-- "framework/languages/node/quickstart/package.json"
```

## 2. Shared contract

The request type must be a class. The packet name comes from `payload.constructor.name`, so
an object literal cannot be used. The reply type is not looked up by name and stays an
`interface`.

```typescript title="Shared/contracts.ts"
--8<-- "framework/languages/node/quickstart/Shared/contracts.ts"
```

## 3. The handling side

A handler is registered in two places — Nest's `providers` and `addRequestHandler` on
`channel(...).server()`. When binding to `0.0.0.0`, `setAdvertiseHost` supplies the address
to advertise; without it startup is rejected.

```typescript title="Server/main.ts"
--8<-- "framework/languages/node/quickstart/Server/main.ts"
```

## 4. The calling side

```typescript title="Client/main.ts"
--8<-- "framework/languages/node/quickstart/Client/main.ts"
```

## 5. Run

```bash
cd framework/languages/node/quickstart
npm install
npm run build

# Two terminals. Start the server first.
npm run server
npm run client

curl http://127.0.0.1:5080/hello/world
```

The response is `"hello, world"` with status 200.

## What to carry over

| File | Content |
|---|---|
| `package.json` | The `@zlink-systems/framework` and `@zlink-systems/nestjs` entries. The binding stays transitive |
| `Shared/contracts.ts` | Request as a class, reply as an `interface` |
| `Server/main.ts` | `addRouteMesh` → `listen` → `setAdvertiseHost` → `channel(...).server().addRequestHandler(...)`, handler in `providers` |
| `Client/main.ts` | `channel(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...).submit<T>()` |

Replacing the manual `peerConnections().connect` with a location store is covered by
[10. Location](guide/server/10-location.en.md).
