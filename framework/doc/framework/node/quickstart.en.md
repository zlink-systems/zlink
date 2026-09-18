# Node.js Quickstart — from Install to a First Request

!!! info "What you get from this chapter"

    You can install the packages and run a minimal project where two processes call each other.

The project lives at
[`framework/languages/node/quickstart/`](../../../languages/node/quickstart/). The code
blocks below are read from those files when the site is built. Without a location store, two
processes name each other's endpoint directly and exchange one request/reply.

## 0. Downloading the tutorial

This chapter builds the smallest project from scratch. **To run the finished tutorial instead**,
one archive is all you need — there is no reason to clone the whole repository.

[**Download zlink-tutorial-node.zip**](https://github.com/zlink-systems/zlink/releases/latest/download/zlink-tutorial-node.zip)

The address does not depend on the platform: Windows and WSL fetch the same file. Unpacking it
leaves the project under `zlink-tutorial-node/`, with the package versions of the release you
downloaded.

For the current main, take just that directory out of the repository.

```bash
git clone --filter=blob:none --sparse https://github.com/zlink-systems/zlink.git
cd zlink
git sparse-checkout set framework/languages/node/tutorial
```

## 1. Installation

- Node.js 22 or later (`engines` of `@zlink-systems/zlink`)
- Access to npm

Get it from npm. The minimal combination needed to build one server is the following.

```bash
npm install @zlink-systems/framework   # The contract and runtime
npm install @zlink-systems/nestjs      # DI/module registration
```

**Do not list `@zlink-systems/zlink` (the binding) yourself.** `@zlink-systems/framework`
declares the version it depends on.

```json title="package.json"
--8<-- "framework/languages/node/quickstart/package.json"
```

Packages to add when you need them:

| Package | When to add it |
| --- | --- |
| `@zlink-systems/framework-locations-redis` | When using the Redis location store for auto-connect ([Location](guide/server/25-location.en.md)) |
| `@zlink-systems/framework-codec-protobuf` · `-codec-msgpack` | To use instead of the default JSON codec ([Handlers and Message Processing](guide/server/31-handler-dispatch.en.md#3-codecs--turning-a-payload-into-bytes)) |
| `@zlink-systems/stream-connector` | When building an external client (a game client, mobile) ([STREAM](guide/server/23-stream.en.md)) |
| `@zlink-systems/http-client` | When the server calls out over HTTP ([HTTP Client guide](guide/http-client/README.en.md)) |

The license differs by layer — core/binding is MPL-2.0, framework is FSL-1.1-ALv2, and
`@zlink-systems/http-client` is Apache-2.0. There is no cost to building and selling a service
([Where ZLink Applies](guide/server/17-alternative.en.md#8-license--the-cost-of-using-it)).

## 2. Shared contract

The request type must be a class. The packet name comes from `payload.constructor.name`, so
an object literal cannot be used. The reply type is not looked up by name and stays an
`interface`.

```typescript title="Shared/contracts.ts"
--8<-- "framework/languages/node/quickstart/Shared/contracts.ts"
```

## 3. The handling side

A handler is registered in two places — Nest's `providers` and `addRequestHandler` on
`channel(...).server()`. When binding to `0.0.0.0` without `setAdvertiseHost`, `127.0.0.1` is advertised; set
`setAdvertiseHost` to a reachable address when other hosts must connect.

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

## 6. What to check when the first run fails

| Symptom | What to check |
| --- | --- |
| A package is not found | Check that the package names in section 1 were copied exactly. The binding version is not pinned separately |
| Startup fails | Check that both processes name the same mesh, and that the listen endpoint does not collide with another process |
| The packet name does not match | Check that the request type is declared as a class. An object literal carries no name |
| A call ends with no target | Check that the receiving side registered that channel name in the server role, and that the handler is also in `providers` |
| No answer arrives | Check that the caller used `request`. A `send` receives no answer |

## 7. What to carry over

| File | Content |
|---|---|
| `package.json` | The `@zlink-systems/framework` and `@zlink-systems/nestjs` entries. The binding stays transitive |
| `Shared/contracts.ts` | Request as a class, reply as an `interface` |
| `Server/main.ts` | `addRouteMesh` → `listen` → `setAdvertiseHost` → `channel(...).server().addRequestHandler(...)`, handler in `providers` |
| `Client/main.ts` | `channel(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...).submit<T>()` |

## 8. What to read next

These two processes connect by writing each other's endpoint directly. Keeping the calling code
unchanged while servers are added or restarted at another address needs automatic connection, and
that is covered by [Location](guide/server/25-location.en.md).

- To go over the concepts first — [Core Concepts](guide/server/03-concepts.en.md)
- The path that calls by name — [Channel Messaging](guide/server/20-channel-messaging.en.md)
- State objects called by id — [Spot](guide/server/21-spot.en.md) · [Actor](guide/server/22-actor.en.md)
- To see a complete business flow — [Picking a Sample](guide/server/14-samples.en.md)
