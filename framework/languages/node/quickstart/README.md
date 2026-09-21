[English](./README.md) | [한국어](./README.ko.md)

# ZLink Node/TypeScript quickstart

This is the smallest project: two processes call each other once over a channel, with no
location store because the client names the server endpoint directly. The site page
`framework/doc/framework/node/quickstart.ko.md` reads its code blocks from these files.
This directory is `quickstart/` in the `zlink-node-examples` repository.

| | Purpose |
|---|---|
| **quickstart** (here) | Installs packages and reaches the first reply |
| tutorial (`tutorial/`) | Adds features one at a time. The feature guides read this code |
| samples (`samples/`) | Shows applications with a complete business flow |

## Prerequisites

- Node.js 22 or newer. The published `@zlink-systems/zlink` binding used by the framework
  requires that runtime version.
- Internet access to `registry.npmjs.org`.
- No Redis or other external service.

## Download and install

Clone the [`zlink-node-examples`](https://github.com/zlink-systems/zlink-node-examples)
repository. Run the commands below from its `quickstart/` directory.

`package.json` pins `@zlink-systems/framework`, `@zlink-systems/nestjs`, `@nestjs/common`,
`@nestjs/core`, `reflect-metadata`, and the TypeScript and `@types/node` versions in
`devDependencies`.
The `@zlink-systems/zlink` binding is not listed; `@zlink-systems/framework` resolves it
transitively.

## Build

```bash title="linux"
npm install
npm run build
```

```powershell title="windows"
npm install
npm run build
```

## Run

Start the server first and the client second in separate terminals. The server listens on
`tcp://0.0.0.0:7101` and handles the `greeting` channel. The client listens on
`tcp://0.0.0.0:7102`, connects to `tcp://127.0.0.1:7101`, and serves
`GET /hello/{name}` on `http://127.0.0.1:5080`.

```bash title="linux"
npm run server > server.log 2>&1 &
npm run client > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5080/hello/world > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:5080/hello/world
```

```powershell title="windows"
Start-Process -NoNewWindow npm.cmd -ArgumentList 'run','server' -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Start-Process -NoNewWindow npm.cmd -ArgumentList 'run','client' -RedirectStandardOutput client.log -RedirectStandardError client.err.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5080/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
$answer
```

## Verify

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5080/hello/world | grep -q '"hello, world"'
echo "quickstart=ok"
```

```powershell title="windows"
$answer = curl.exe -sf http://127.0.0.1:5080/hello/world
if ($LASTEXITCODE -ne 0 -or $answer -notmatch '"hello, world"') { throw 'quickstart failed' }
Write-Output 'quickstart=ok'
```

The endpoint returns `"hello, world"` with HTTP status 200.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Ports 7101, 7102, or 5080 are busy | Stop the earlier Server or Client process |
| The curl request cannot connect | Start Server, then Client, and inspect process output |
| The request has no target | Match `peerConnections().connect` and `listen` endpoints |
| The request fails before the handler runs | Send `new Hello(name)`, not an object literal |
| The handler is not called | Register `HelloHandler` and list it in `providers` |
| Startup rejects a wildcard endpoint | Keep `.setAdvertiseHost(...)` after `.listen(...)` |

## Project layout

| Path | Contents |
|---|---|
| `Shared/contracts.ts` | The `Hello` class and `Greeting` contract shared by both processes |
| `Server/main.ts` | Registers the `greeting` handler and listens on port 7101 |
| `Client/main.ts` | Connects to the server and exposes `GET /hello/{name}` on port 5080 |
| `package.json` | Runtime and development package pins and the three npm scripts |

## What to carry into your own project

- The exact pins in `package.json`. Keep `@zlink-systems/zlink` transitive unless the
  framework package contract requires a direct dependency.
- `Shared/contracts.ts`: make the request payload a class so packet-name resolution has a
  concrete constructor. Keep the reply contract aligned with the decoded shape.
- `Server/main.ts`: preserve the `ZLinkModule.forRootFactory` order, the mesh name,
  `.listen(...)`, `.setAdvertiseHost(...)`, `channel(...).server().addRequestHandler(...)`,
  and the handler class in `providers`.
- `Client/main.ts`: preserve `channel(...).client()`,
  `peerConnections().connect(...)`, the `ZLINK_ROUTE_CLIENT` lookup, and
  `requestToChannel(...).submit<Greeting>()`.
- A production service normally replaces the manual peer connection with a location store,
  such as Redis. This quickstart omits that service dependency.
