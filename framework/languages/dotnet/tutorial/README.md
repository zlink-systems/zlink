# .NET Tutorial

A program that the feature-by-feature guide reads code out of. Follow the
chapters one at a time and this program grows in that same order.

This tutorial runs from `tutorial/` in the `zlink-dotnet-examples` repository. The
Korean canonical version, including the full chapter-by-chapter walkthrough,
the code-snippet marker table the docs generator reads, and the list of
places this code corrects the guide text, is [README.ko.md](README.ko.md).

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

- **.NET SDK 8.0 or later** -- `dotnet --version` must report an `8.0.x` (or
  newer) SDK.
- **Docker Desktop** (or another Docker Engine) -- rooms, queues, and players
  need a Location Store to record where they live. Channel messaging alone
  does not need one.

**Linux · macOS · WSL — bash**

```bash title="linux"
docker run --rm -d -p 6379:6379 --name zlink-tutorial-dotnet-redis redis:7.2-alpine
```

**Windows — PowerShell 7**

```powershell title="windows"
docker run --rm -d -p 6379:6379 --name zlink-tutorial-dotnet-redis redis:7.2-alpine
```

## Download and install

The tutorial references only the published `Zlink.Framework` NuGet package. Clone the
`zlink-dotnet-examples` repository and run this file's commands from its `tutorial/`
directory (a repository checkout runs the same commands from
`framework/languages/dotnet/tutorial`). The first build's implicit `dotnet restore` fetches
that package from nuget.org.

## Build

This keeps the code the docs show and the library a reader gets from
nuget.org the same thing. Building inside the repository still references
the `Zlink.Framework` package, and copying just this directory out still
builds the same way.

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet build Tutorial.sln -c Release
```

**Windows — PowerShell 7**

```powershell title="windows"
dotnet build Tutorial.sln -c Release
```

Switch to this only when you need to diff against the repository source
(only works inside a repository checkout):

```bash
dotnet build Tutorial.sln -c Release -p:ZLinkTutorialUseLocalSource=true
```

The package version lives in
[`Directory.Packages.props`](Directory.Packages.props), and
`scripts/local-package/sync-version.py` keeps it in sync with `../VERSION`.

## Run

Start the Server in the background first, then the Client, which opens HTTP
on top of it. Once both are ready, one request confirms they are connected,
and this leaves that result in a file the next section reads.

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet run --project Server/Server.csproj -c Release --no-build > server.log 2>&1 &
echo $! > server.pid
dotnet run --project Client/Client.csproj -c Release --no-build > client.log 2>&1 &
echo $! > client.pid
for _ in $(seq 1 60); do
  curl -sf http://127.0.0.1:5080/players/warmup/profile >/dev/null 2>&1 && break
  sleep 1
done
```

**Windows — PowerShell 7**

```powershell title="windows"
$server = Start-Process dotnet -ArgumentList "run","--project","Server/Server.csproj","-c","Release","--no-build" `
  -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru -WindowStyle Hidden
$server.Id | Out-File server.pid
$client = Start-Process dotnet -ArgumentList "run","--project","Client/Client.csproj","-c","Release","--no-build" `
  -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru -WindowStyle Hidden
$client.Id | Out-File client.pid
for ($i = 0; $i -lt 60; $i++) {
  try { Invoke-RestMethod -Uri "http://127.0.0.1:5080/players/warmup/profile" -TimeoutSec 2 | Out-Null; break }
  catch { Start-Sleep -Seconds 1 }
}
```

## Verify

Examples smoke runs this block exactly as written.

A `/players/p1/profile` response containing `"playerId":"p1"` means the Server and
Client found each other (the same request "Run" issued). Once confirmed, stop both
processes.

**Linux · macOS · WSL — bash**

```bash title="linux"
curl -sf http://127.0.0.1:5080/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
$profile = Invoke-RestMethod -Uri 'http://127.0.0.1:5080/players/p1/profile'
if ($profile.playerId -ne 'p1') { throw "tutorial verify failed: $($profile | ConvertTo-Json -Compress)" }
Write-Output 'tutorial-http=ok'
```

Each feature's own `curl` command under the Korean canonical's chapter
walkthrough must return the response written next to it for that feature to
count as working. To check the whole tutorial the way CI does, follow
[`.github/workflows/framework-tutorial.yml`](https://github.com/zlink-systems/zlink/blob/main/.github/workflows/framework-tutorial.yml)
in order -- its last line after every step passes is `all tutorial steps
passed`.

## Stop

Stop the processes started by the Run section.

**Linux · macOS · WSL — bash**

```bash title="linux"
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
docker rm -f zlink-tutorial-dotnet-redis 2>/dev/null || true
```

**Windows — PowerShell 7**

```powershell title="windows"
Get-Content client.pid, server.pid | ForEach-Object {
  if ($_ -match '^\d+$') { taskkill /PID $_ /T /F 2>$null | Out-Null }
}
Get-Job | Stop-Job -ErrorAction SilentlyContinue
docker rm -f zlink-tutorial-dotnet-redis 2>$null | Out-Null
```

## Troubleshooting

- **Docker is not running / cannot connect to Redis** -- start Docker Desktop
  (or your Docker Engine) and start Redis again with the `docker run` command
  under Prerequisites.
- **Port 6379 is already in use** -- check whether an earlier run's Redis
  container is still up with `docker ps`. Unlike the samples, this tutorial
  uses a fixed `redis://127.0.0.1:6379`, so it expects exactly one such
  container reused across runs (`docker rm -f zlink-tutorial-dotnet-redis`
  then start it again for a clean state).
- **`dotnet` reports no compatible SDK** -- install the .NET 8.0 (or newer)
  SDK.
- **RID registration is rejected with `RejectedConflict`** -- an earlier
  run's stale keys are still in the same Redis. Clear only the keys this
  tutorial uses (leave any other data in that Redis alone if you share it
  with something else):

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial:*' | xargs -r redis-cli del
  ```

## HttpClient

`HttpClient` is an external HTTP client program that calls the tutorial Client
and Server surfaces through `ZLinkHttpClient`. It references only the
`Zlink.HttpClient` package, so the calls are usable as source snippets in the
feature guides without referencing Framework internals.

With Server and Client running, build and run it as follows:

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet build Tutorial.sln -c Release
dotnet run --project HttpClient/HttpClient.csproj -c Release --no-build
```

**Windows — PowerShell 7**

```powershell title="windows"
dotnet build Tutorial.sln -c Release
dotnet run --project HttpClient/HttpClient.csproj -c Release --no-build
```

The recorded output is:

```
first request: p1 rookie
request shaping: status 200 weight 2
json body: player 200 room 4400f753-2bf4-45dc-aa7e-84714c27822e chat 202
response kinds: typed 200 raw application/json; charset=utf-8 fetch anonymous
compressed response: 200 encoding-removed True
redirect: 200 p1
basic auth: without 401 with 200
download stream: chunks 2 bytes 74
upload stream: imported 3
error kinds: bad request InternalFailure connection refused Unavailable
```

The operational routes can be checked with these requests. The admin route
requires Basic auth; without it the response is 401 with
`WWW-Authenticate: Basic realm="tutorial-admin"`.

```bash
curl -i -u ops:tutorial-admin -X POST \
  "http://127.0.0.1:5081/admin/channels/profile/weight?value=2"
# 200 {"channel":"profile","weight":2}

curl -i http://127.0.0.1:5081/fanout/broadcast/ready
# 200 when the broadcast subscriber is ready; otherwise 503

curl -i http://127.0.0.1:5080/player/p1
# 301 Location: /players/p1

curl -i -H 'Accept-Encoding: gzip' http://127.0.0.1:5080/rooms/$ROOM
# 200 Content-Encoding: gzip

curl -i http://127.0.0.1:5080/rooms/$ROOM/export
# 200 Content-Type: application/x-ndjson
# {"roomId":"..."}
# {"message":"p1: hello"}

curl -X POST http://127.0.0.1:5080/rooms/$ROOM/import \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary $'{"playerId":"p2","text":"one"}\n{"playerId":"p2","text":"two"}\n{"playerId":"p2","text":"three"}\n'
# {"imported":3}
```

The source marker table for the HTTP client is:

| Marker | Source |
|---|---|
| `http-client-create` | `HttpClient/Program.cs` |
| `http-first-request` | `HttpClient/Program.cs` |
| `http-request-shaping` | `HttpClient/Program.cs` |
| `http-json-body` | `HttpClient/Program.cs` |
| `http-response-kinds` | `HttpClient/Program.cs` |
| `http-compressed-response` | `HttpClient/Program.cs` |
| `http-redirect` | `HttpClient/Program.cs` |
| `http-basic-auth` | `HttpClient/Program.cs` |
| `http-download-stream` | `HttpClient/Program.cs` |
| `http-upload-stream` | `HttpClient/Program.cs` |
| `http-error-kinds` | `HttpClient/Program.cs` |
