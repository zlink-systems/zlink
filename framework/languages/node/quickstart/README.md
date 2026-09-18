# ZLink Node/TypeScript quickstart

The project that `framework/doc/framework/node/quickstart.ko.md` reads from, built as a
project that actually installs and runs. No Redis, no location store — a server process handles the `greeting` channel and a
client process calls it through a manually-configured peer connection. Builds from npm.org
only, outside this repository too.

## Prerequisites

- **Node.js 20 or newer to install `@zlink-systems/framework`/`@zlink-systems/nestjs`, but
  Node.js 22 or newer to actually run this project.** The guide's own §1 install table says
  "Node.js 20 이상", and neither `@zlink-systems/framework@0.12.0` nor
  `@zlink-systems/nestjs@0.12.0` declares an `engines` field (checked via
  `npm view @zlink-systems/framework@0.12.0 engines` /
  `npm view @zlink-systems/nestjs@0.12.0 engines` — both print nothing). But
  `@zlink-systems/framework@0.12.0` pins a hard dependency on
  `@zlink-systems/zlink@1.1.0` (the native binding, the same version this repo's own
  `bindings/node/package.json` publishes), and that package's `package.json` declares
  `"engines": { "node": ">=22" }` — confirmed by
  `npm view @zlink-systems/zlink@1.1.0 engines` → `{ node: '>=22' }`. The floor is real: the
  native module the binding loads only ships prebuilds new enough for Node 22's ABI.
  Installed here: Node `v22.23.2`, npm `10.9.8`.
- Internet access (registry.npmjs.org). No `.npmrc`/`nuget.config`-equivalent is needed —
  this project uses the default public registry.
- No Redis or other external dependency.

## Layout

- `Shared/contracts.ts` — the `Hello`/`Greeting` contract both processes import.
- `Server/main.ts` — handles the `greeting` channel. Listens on `tcp://0.0.0.0:7101`.
- `Client/main.ts` — calls `greeting` and exposes `GET /hello/{name}` on a plain
  `node:http` server listening on `http://127.0.0.1:5080`. Connects to the server manually
  at `tcp://127.0.0.1:7101`.
- `package.json` — pins `@zlink-systems/framework`, `@zlink-systems/nestjs`,
  `@nestjs/common`, `@nestjs/core`, `reflect-metadata` to explicit numbers in one place
  (no ranges, no conditionals).

## First — the published versions actually used

**Only packages actually published to npm.** Checked `https://registry.npmjs.org/@zlink-systems%2Fframework`
and `.../@zlink-systems%2Fnestjs` before pinning anything:

| Package | Published versions | `dist-tags.latest` |
| --- | --- | --- |
| `@zlink-systems/framework` | `0.10.0`, `0.11.0`, `0.12.0` | `0.12.0` |
| `@zlink-systems/nestjs` | `0.10.0`, `0.11.0`, `0.12.0` | `0.12.0` |
| `@zlink-systems/zlink` | `0.17.3`, `0.17.5`, `0.17.6`, `0.18.0`, `1.1.0` | `1.1.0` |

`framework/languages/node/samples/*/package.json` pin `@zlink-systems/framework@0.11.1` /
`@zlink-systems/nestjs@0.11.1` — **`0.11.1` does not exist on npm** (only `0.10.0`, `0.11.0`
and `0.12.0` do). Those samples build with `scripts/prepare-dependencies.mjs`, which points
at a local/source package instead of installing from the registry — that is exactly the
unpublished-version trap the brief warns about. This quickstart does not use that script or
those version numbers.

Like the .NET quickstart's nuspec check, I read what the framework package actually depends
on instead of guessing a binding version: `npm view @zlink-systems/framework@0.12.0
dependencies` → `@zlink-systems/zlink: '1.1.0'` (an exact pin, not a range; `0.11.0` pinned
`0.17.6` the same way — the exact pin moved with the framework version, it did not become a
range). `npm install` resolves the binding to `1.1.0` transitively — `@zlink-systems/zlink`
is deliberately **not** listed in this project's `package.json`, and staying off that list
is still correct at `0.12.0`: there is no reason left to hand-pin a binding version the
framework package already pins exactly. `npm view @zlink-systems/nestjs@0.12.0 dependencies`
/ `peerDependencies` → depends on `@zlink-systems/framework: '0.12.0'` (exact) and
`@nestjs/common: '^10.4.22'`, and peer-depends on `@nestjs/core: '^10.4.22'`. This project
pins `@nestjs/common`/`@nestjs/core` to `10.4.22` explicitly (the peer dependency is not
auto-installed with a pinned version otherwise) and `reflect-metadata` to `0.2.2` (satisfies
the `^0.2.2` range `@zlink-systems/nestjs@0.12.0` itself depends on).

## Build and run

```bash
cd framework/languages/node/quickstart
npm install
npm run build

# two terminals (or background)
npm run server
npm run client

# once both are up
curl http://127.0.0.1:5080/hello/world
```

## Actual output

```
$ npm install
added 44 packages, and audited 45 packages in 6s
$ npm run build
> zlink-quickstart@0.0.0 build
> tsc -p tsconfig.json
$ node dist/Server/main.js
[Nest] ... LOG [NestFactory] Starting Nest application...
[Nest] ... LOG [InstanceLoader] ServerModule dependencies initialized
[Nest] ... LOG [InstanceLoader] DiscoveryModule dependencies initialized
[Nest] ... LOG [InstanceLoader] ZLinkModule dependencies initialized
server listening on tcp://0.0.0.0:7101 (channel "greeting")
$ node dist/Client/main.js
[Nest] ... LOG [NestFactory] Starting Nest application...
[Nest] ... LOG [InstanceLoader] ClientModule dependencies initialized
[Nest] ... LOG [InstanceLoader] DiscoveryModule dependencies initialized
[Nest] ... LOG [InstanceLoader] ZLinkModule dependencies initialized
client listening on http://127.0.0.1:5080
$ curl -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5080/hello/world
"hello, world"
HTTP_STATUS:200
```

`resolveFrameworkPacketName` traced in
`framework/languages/node/packages/framework/src/runtime/messaging/packet-name.ts` (read for
understanding only — this quickstart does not import repo source, see "Forbidden" below) is
what makes the codec-level failure mode explained below reproducible, not a guess.

## What to take into your own project

- The `package.json` `dependencies` block's exact pins and the reasoning for which ones are
  pinned versus left to transitive resolution (`@zlink-systems/zlink` is not pinned here).
- `Shared/contracts.ts`'s pattern: the request type is a `class`, not a plain `interface`,
  because request routing depends on it (see "Differences" below).
- `Server/main.ts`'s `ZLinkModule.forRootFactory` block — mesh name, `.listen(...)`,
  `.setAdvertiseHost(...)`, and `mesh.channel(...).server().addRequestHandler(...)` order,
  plus listing the handler class in the Nest module's own `providers`.
- `Client/main.ts`'s `ZLinkModule.forRootFactory` block — `mesh.channel(...).client()`,
  `mesh.peerConnections().connect(...)`, and reading `ZLINK_ROUTE_CLIENT` off the Nest
  application context with `app.get(ZLINK_ROUTE_CLIENT, { strict: false })`, then calling
  `route.requestToChannel(channelName, new Hello(...)).submit<Greeting>()`.
- In a real service, manual `mesh.peerConnections().connect(...)` is normally replaced by a
  location store (Redis, etc.) — left out here on purpose; this quickstart only confirms the
  install works.

## Differences from guide §2 (needed to actually run it)

The guide's snippet is deliberately trimmed. Reproducing it as-is hits four separate
problems; the doc itself is out of scope for this job (Job Q-NODE) so it was not changed —
only this runnable project was adjusted, the same way the .NET quickstart adjusted its own
three points:

1. **The request payload must be a `class`, not a plain object literal.** The guide's
   `Hello`/`Greeting` are TypeScript `interface`s, and its client call is
   `this.route.requestToChannel('greeting', { name }).submit<Greeting>()`. Traced this
   through the source: `requestToChannel(channelName, payload)` derives the wire *packet
   name* it dispatches by from `payload.constructor.name`
   (`resolveFrameworkPacketName` → `tryConstructorPacketName` in
   `packages/framework/src/runtime/messaging/packet-name.ts`), and explicitly rejects
   `'Object'` (and `Array`/`String`/etc.) as a source — a plain `{ name }` literal's
   constructor is `Object`, so packet-name resolution fails and
   `resolveFrameworkPacketName` throws `ZLinkConfigurationException: ... packetName is
   required when the payload type cannot provide one.` `requestToChannel`'s own signature
   (`contracts/Channels/RouteCalls.ts`) has no separate packet-name parameter to work around
   this. This quickstart's `Shared/contracts.ts` makes `Hello` a `class` instead, and the
   handler is registered under the packet name `'Hello'` (matching the class name) with
   `mesh.channel('greeting').server().addRequestHandler(PacketNames.hello, HelloHandler)` —
   the same explicit-registration style the guide already uses for the handler side. This
   matches how the framework's own tested code does it: a message class instance is
   sent, not an object literal.
2. **A wildcard bind host needs an explicit advertise host.** The guide listens with
   `.listen('tcp://0.0.0.0:7101')` and stops there. Doing exactly that throws at startup:
   `ZLinkConfigurationException: SpotNode 'services' router must define an advertise host
   when its bind host is a wildcard address.` (`contracts/Configuration/
   RegistrationValidators.ts`, `validateListenerNetworkIdentity`). Both `Server/main.ts` and
   `Client/main.ts` add `.setAdvertiseHost('127.0.0.1')` right after `.listen(...)`.
3. **The client needs its own real HTTP server, and it isn't NestJS's HTTP module.** The
   guide's client tab shows a NestJS `@Controller`/`@Get('/hello/:name')` HTTP handler
   injecting `ZLINK_ROUTE_CLIENT`. Nowhere in this repository's own Node samples, tests
   or packages is `@Controller`/`@Get` actually used (checked with
   `grep -rl '@Controller(\|@Get('` across `packages/`, `samples/`, `test/` — no
   hits) — every process that needs HTTP in this codebase instead calls
   `NestFactory.createApplicationContext(...)` (a DI-only Nest context, no HTTP listener)
   and layers a plain `node:http` server on top to reach `ZLINK_ROUTE_CLIENT` through
   `app.get(...)`. `Client/main.ts` follows that same tested shape rather than the
   `@Controller` snippet, which would also need adding `@nestjs/platform-express` (not a
   dependency of either `@zlink-systems/nestjs` or this project) with no proof in this
   codebase that it is exercised.
4. **`addRequestHandler` needs Nest DI, not decorator auto-discovery.** As in the .NET
   quickstart's own finding 2, the guide's `mesh.channel('greeting').server()
   .addRequestHandler(PacketNames.hello, HelloHandler)` line is registered explicitly, and
   `HelloHandler` is also listed in the enclosing `@Module`'s `providers: [HelloHandler]` so
   Nest's injector can construct it. The guide's other Node tabs also show
   `zlinkModule(__dirname, {})`-based decorator auto-discovery
   (`@zlinkRequestHandler(...)` on the handler class); that path scans **compiled** `.js`
   files at the given directory at runtime (`loadDecoratedProviderModules` in
   `packages/nestjs/src/module.ts` filters on `\.(?:cjs|mjs|js)$`), which only works once
   `Server/main.ts`'s directory has been built to `dist/Server/`. The explicit-registration
   form used here has the same build-order requirement in principle but doesn't depend on
   post-build directory scanning, so it was kept for a two-and-a-half-file quickstart.
