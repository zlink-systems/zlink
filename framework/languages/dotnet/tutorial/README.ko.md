# .NET Tutorial

기능별 guide가 코드를 읽는 프로그램이다. 각 장은 이전 단계에 기능을 추가한다.


이 문서는 `zlink-dotnet-examples` 저장소의 `tutorial/`에서 실행한다. 영어 대응 문서는
[README.md](README.md)다.

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

- **.NET SDK 8.0 이상** — `dotnet --version`이 `8.0.x` 이상 SDK를 보고해야 한다.
- **Docker Desktop**(또는 다른 Docker Engine) — 방·queue·player는 위치를 기록할
  Location Store가 있어야 동작한다. Channel 메시징에는 필요 없다.

**Linux · macOS · WSL — bash**

```bash title="linux"
docker run --rm -d -p 6379:6379 --name zlink-tutorial-dotnet-redis redis:7.2-alpine
```

**Windows — PowerShell 7**

```powershell title="windows"
docker run --rm -d -p 6379:6379 --name zlink-tutorial-dotnet-redis redis:7.2-alpine
```

## 내려받기와 설치

tutorial은 공개된 `Zlink.Framework` NuGet 패키지만 참조한다. `zlink-dotnet-examples`
저장소를 clone하고 이 문서의 명령을 `tutorial/`에서 실행한다(저장소 checkout이면 같은
명령을 `framework/languages/dotnet/tutorial`에서 실행한다). 첫 빌드에서 `dotnet build`가
암묵적으로 실행하는 `dotnet restore`가 그 패키지를 nuget.org에서 내려받는다.

## 빌드

문서의 코드와 nuget.org에서 받는 라이브러리는 같은 package를 사용한다.
저장소에서 빌드해도 `Zlink.Framework` package를 참조하며, 이 디렉터리만 복사해도
빌드할 수 있다.

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet build Tutorial.sln -c Release
```

**Windows — PowerShell 7**

```powershell title="windows"
dotnet build Tutorial.sln -c Release
```

저장소 소스와 대조해야 할 때만 아래로 바꾼다(저장소 checkout에서만 동작한다).

```bash
dotnet build Tutorial.sln -c Release -p:ZLinkTutorialUseLocalSource=true
```

패키지 버전은 [`Directory.Packages.props`](Directory.Packages.props)에 있고
`scripts/local-package/sync-version.py`가 `../VERSION`에 맞춰 갱신한다.

## 실행

Server를 백그라운드 process로 먼저 실행하고, Client가 HTTP를 제공한다. 연결이 준비되면
요청으로 상태를 확인하고 결과를 다음 절에서 읽을 파일에 기록한다.

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

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

`/players/p1/profile` 응답에 `"playerId":"p1"`이 있으면 Server와 Client 연결이 준비된
상태다("실행" 절과 같은 요청이다). 확인 후 두 process를 종료한다.

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

"## 단계" 아래 각 기능의 curl 명령도 적힌 응답을 반환해야 한다. 전체를 자동으로 확인하려면
[`.github/workflows/framework-tutorial.yml`](https://github.com/zlink-systems/zlink/blob/main/.github/workflows/framework-tutorial.yml)의
순서를 그대로 따라간다 — 모든 단계를 통과하면 마지막 줄이 `all tutorial steps
passed`다.


## 종료

실행 절에서 시작한 process를 종료한다.

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

## IDE에서 열기

Rider 또는 Visual Studio 2022·2026에서 `Tutorial.sln`을 연다. `Server`를 먼저, `Client`를
다음으로 하는 `Multiple startup projects` 구성으로 함께 실행하거나 각 project를 그 순서로
개별 실행한다. 종료는 IDE의 Stop 버튼으로 한다.

## 문제 해결

- **Docker가 실행 중이 아니다 / Redis에 연결할 수 없다** — Docker Desktop(또는
  Docker Engine)을 시작하고 위 "전제 조건"의 `docker run` 명령으로 Redis를 다시
  실행한다.
- **6379 포트가 이미 쓰이고 있다** — `redis-cli -h 127.0.0.1 -p 6379 ping`의 응답이
  `PONG`이면 기존 Redis를 사용할 수 있다. 이 경우 위 `docker run` 명령을 생략하고,
  이전 tutorial process를 종료한 뒤 아래 `zlink-tutorial:*` 키 정리 명령을 실행한다.
  그런 다음 Server와 Client를 다시 실행한다. 기존 Redis를 사용했다면 "종료" 절의
  `docker rm` 명령은 실행하지 않는다.
- **`dotnet`이 호환되는 SDK가 없다고 한다** — .NET 8.0 이상 SDK를 설치한다.
- **RID 등록이 `RejectedConflict`로 거부된다** — 이전 실행이 남긴 오래된 키가 같은
  Redis에 남아 있을 때 나타난다. Server와 Client를 종료한 뒤 이 tutorial의
  `zlink-tutorial:*` 키만 지우고 다시 실행한다. 같은 Redis의 다른 키는 지우지 않는다.

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial:*' | xargs -r redis-cli del
  ```

  Windows에서는 같은 Redis에 연결된 `redis-cli`로 다음 명령을 실행한다.

  ```powershell
  redis-cli --scan --pattern 'zlink-tutorial:*' | ForEach-Object { redis-cli DEL $_ | Out-Null }
  ```

## 프로젝트

| 프로젝트 | 역할 |
|---|---|
| `Shared` | 양쪽 process가 함께 사용하는 message 계약 |
| `Server` | channel handler, 방·queue, player, client session을 실행한다 |
| `Client` | HTTP 요청을 받아 mesh로 호출한다. 방과 player를 생성하고 호출한다 |
| `StreamClient` | 외부 TCP client. framework 없이 connector만 참조한다 |
| `HttpClient` | 외부 HTTP client. framework 없이 http-client만 참조한다 |

## quickstart·샘플과 나눠 두는 이유

| | 목적 |
|---|---|
| [`../quickstart/`](../quickstart/) | 설치와 첫 응답 확인. 기능을 추가하지 않는다 |
| **`tutorial/`** (여기) | 기능을 단계별로 추가한다. 기능별 guide가 이 코드를 읽는다 |
| [`../samples/`](../samples/) | 완결된 업무 흐름을 보이는 application |

tutorial은 기능마다 필요한 최소 구성만 담는다. 도메인 로직을 추가하면 sample의 축소판이 된다.

## 단계

각 기능은 독립적으로 읽을 수 있다. 앞 단계를 실행하지 않아도 다음 단계가 동작한다.

### 1. Channel 메시징 — RouteMesh

요청하는 쪽이 node를 고르지 않는다. 채널 이름만 주면 그 채널을 담당하는 node가 받는다.

```bash
curl http://127.0.0.1:5080/players/p1/profile
# {"playerId":"p1","nickname":"rookie","level":1}

curl -X POST http://127.0.0.1:5080/players/p1/logins
# 202. Server 로그에 login recorded: p1
```

두 번째는 응답을 기다리지 않는 단방향 호출이다.

### 2. Channel 메시징 — node 직접 호출

channel을 거치지 않는 경로다. 받는 쪽은 `mesh.AddRouteRequestHandler`로 mesh에 바로 등록하고,
부르는 쪽은 node의 routing id를 지정한다. 운영 명령에만 쓴다.

```bash
curl http://127.0.0.1:5080/ops/nodes/game-server-1/status
# {"meshName":"game","channelName":"(none)",
#  "calledBy":"game-2a0af167-...","uptime":"14s","processId":39892}

curl -i http://127.0.0.1:5080/ops/nodes/no-such-node/status
# 404 {"error":"not_found",
#      "message":"Route channel 'game' does not know node 'no-such-node' for packet 'GetNodeStatus'."}
# channel 호출과 달리 후보를 고르지 않으므로 그대로 실패한다.
```

`channelName`이 비어 있으면 channel이 관여하지 않았음을 나타낸다. `calledBy`는
호출한 node의 routing id이고, 나머지 값은 응답한 process의 값이다.

이 호출에서는 받는 node가 `SetRoutingId`로 id를 고정해야 한다. 고정하지
않으면 Framework가 만든 id가 붙어 호출하는 쪽이 URL에 지정할 수 없다.

호출하는 쪽은 해당 node와 peer로 연결되어 있어야 한다. `Connect(RoutingId, endpoint)`로 기대하는
id를 함께 지정할 수 있지만, 이는 연결을 해당 node로 제한할 뿐 node 직접 호출의 조건은 아니다.

### 3. Channel 메시징 — ClientServer

호출 코드는 위와 같다. 다른 것은 **누가 받느냐**다. 부르는 쪽이 연결한 서버가 받는다.

```bash
curl -X POST http://127.0.0.1:5080/players/p1/tickets
# "ticket-p1"
```

### 4. Channel 메시징 — Fanout

보내는 쪽이 받는 node를 모른다. 구독한 node가 모두 받는다.

```bash
curl -X POST http://127.0.0.1:5080/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# 202. Server 로그에 maintenance notice: scheduled maintenance
```

### 5. User Spot — 만들어서 쓰는 방

방을 생성하고 id를 받는다. 이후에는 그 id로 호출한다.

```bash
ROOM=$(curl -s -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"bronze-1"}' | tr -d '"')

curl -X POST http://127.0.0.1:5080/rooms/$ROOM/chat \
  -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'

curl http://127.0.0.1:5080/rooms/$ROOM
# {"title":"bronze-1","chat":["p1: hello"]}
```

방은 호출 사이에 상태를 유지한다.

### 6. Instance Spot — 첫 메시지가 만드는 큐

만드는 호출이 없다. 그 id로 첫 메시지가 도착하면 Framework가 만들고 같은 메시지를 처리한다.

```bash
curl -X POST http://127.0.0.1:5080/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p1"}'
# {"waiting":1}

curl -X POST http://127.0.0.1:5080/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p2"}'
# {"waiting":2}
```

queue는 값을 유지한다. 같은 id로 다시 호출하면 숫자가 이어진다. 처음부터 확인하려면
다른 id를 사용한다.

### 7. Actor — id로 부르는 플레이어

```bash
curl -X POST http://127.0.0.1:5080/players/p1 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"  — 같은 호출을 다시 하면 "existing"

curl -X POST http://127.0.0.1:5080/players/p1/nickname \
  -H 'Content-Type: application/json' -d '{"nickname":"rocket"}'

curl http://127.0.0.1:5080/players/p1
# {"playerId":"p1","nickname":"rocket"}
```

### 8. STREAM과 Session-Actor 연결

외부 client가 TCP로 붙는다. framework가 아니라 connector만 참조한다.

```bash
dotnet run --project StreamClient/StreamClient.csproj
```

```
connected: True
round trip: 33ms          # STREAM request/reply
actor bound: p1
bound player: p1          # 연결에 Actor가 하나인 상태
pushed: speedy, actor: p1 # connector에서 handle 없이 전송·수신
actor bound: p2
bound player: p2          # 같은 연결에 두 번째 캐릭터를 묶는다
actor handle: p1
actor handle: p2
received actor id: p1
received actor id: p2
pushed: speedy-p1, actor: p1
pushed: speedy-p2, actor: p2
```

`pushed`는 nickname 변경 요청의 응답이 아닌 **각 player가 같은 연결로 보낸 알림**이다.
첫 요청은 Actor가 하나라 connector에서 handle 없이 보낸다. 둘을 묶은 뒤에는 Actor handle로
대상을 지정하고 handle별 수신 callback으로 알림을 받는다. connector 수신 callback에서도
`ActorId`를 읽어 같은 알림의 발신 Actor를 구분할 수 있다.

### 9. HTTP client

`HttpClient`는 tutorial Client와 Server가 제공하는 HTTP 표면을 `ZLinkHttpClient`로 호출하는 외부
프로그램이다. framework 패키지는 참조하지 않고 `Zlink.HttpClient`만 참조하며, 각 기능의 호출 코드는
아래 마커에서 기능별 http-client 가이드가 읽는다.

Server와 Client를 실행한 상태에서 다음 명령으로 빌드하고 실행한다.

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

실행 결과는 다음과 같다.

```
first request: p1 rookie
request shaping: status 200 weight 2
json body: player 200 room 4400f753-2bf4-45dc-aa7e-84714c27822e chat 202
response kinds: typed 200 raw application/json; charset=utf-8 fetch speedy-p2
compressed response: 200 encoding-removed True
redirect: 200 p1
basic auth: without 401 with 200
download stream: chunks 2 bytes 74
upload stream: imported 3
error kinds: bad request InternalFailure connection refused Unavailable
```

운영 route는 다음 명령으로 확인한다. admin route는 Basic auth가 없으면 401과
`WWW-Authenticate: Basic realm="tutorial-admin"`을 반환하고, 올바른 자격 증명이 있으면 weight를
변경한다.

```bash
curl -i -u ops:tutorial-admin -X POST \
  "http://127.0.0.1:5081/admin/channels/profile/weight?value=2"
# 200 {"channel":"profile","weight":2}

curl -i http://127.0.0.1:5081/fanout/broadcast/ready
# broadcast subscriber가 준비되었으면 200, 그렇지 않으면 503

curl -i http://127.0.0.1:5080/player/p1
# 301 Location: /players/p1

curl -i -H 'Accept-Encoding: gzip' \
  http://127.0.0.1:5080/rooms/$ROOM
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

### 10. 모니터링

```bash
curl http://127.0.0.1:5080/status
# {"state":"Serving","ready":true,"acceptingWork":true,"safeToShutdown":true,...}
```

## 문서가 읽는 방식

문서는 코드를 직접 옮겨 적지 않고 이 파일에서 구간을 읽는다. 구간은 소스의
`--8<--` 마커가 정한다.

```
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
```

마커 이름을 바꾸면 해당 구간을 읽는 문서가 빈 코드 블록을 출력한다. 이름을 바꿀 때는
문서도 함께 수정한다. CI는 아래 표와 소스를 대조한다.

| 마커 | 자리 |
|---|---|
| `channel-contracts` | `Shared/Contracts.cs` |
| `channel-request-handler` | `Server/Channel/GetPlayerProfileHandler.cs` |
| `channel-send-handler` | `Server/Channel/RecordLoginHandler.cs` |
| `mesh-register` | `Server/Program.cs` |
| `channel-register` | `Server/Program.cs` |
| `channel-client-register` | `Client/Program.cs` |
| `channel-request-call` | `Client/Program.cs` |
| `channel-send-call` | `Client/Program.cs` |
| `node-direct-contracts` | `Shared/Contracts.cs` |
| `node-direct-handler` | `Server/Ops/NodeStatusHandler.cs` |
| `node-direct-register` | `Server/Program.cs` |
| `node-direct-call` | `Client/Program.cs` |
| `filter-implementation` | `Server/Dispatch/CallLogFilter.cs` |
| `filter-register` | `Server/Program.cs` |
| `weight-runtime` | `Server/Program.cs` |
| `clientserver-contracts` | `Shared/Contracts.cs` |
| `clientserver-handler` | `Server/Channel/IssueSessionTicketHandler.cs` |
| `clientserver-register` | `Server/Program.cs` |
| `clientserver-client-register` | `Client/Program.cs` |
| `clientserver-call` | `Client/Program.cs` |
| `fanout-contracts` | `Shared/Contracts.cs` |
| `fanout-handler` | `Server/Channel/MaintenanceNoticeSubscriber.cs` |
| `fanout-subscribe` | `Server/Program.cs` |
| `fanout-publish-register` | `Client/Program.cs` |
| `fanout-call` | `Client/Program.cs` |
| `location-store` | `Server/Program.cs` |
| `relocation-store` | `Server/Program.cs` |
| `location-store-client` | `Client/Program.cs` |
| `object-server` | `Server/Program.cs` |
| `spot-contracts` | `Shared/Contracts.cs` |
| `spot-class` | `Server/Spots/GameRoom.cs` |
| `spot-handlers` | `Server/Spots/GameRoomHandlers.cs` |
| `spot-register` | `Server/Program.cs` |
| `spot-client-register` | `Client/Program.cs` |
| `spot-create-call` | `Client/Program.cs` |
| `spot-message-call` | `Client/Program.cs` |
| `spot-send-call` · `spot-request-call` | `Client/Program.cs`. `spot-message-call` 안에 나뉘어 있다 |
| `instance-spot-contracts` | `Shared/Contracts.cs` |
| `instance-spot-class` | `Server/Spots/MatchQueue.cs` |
| `instance-spot-handler` | `Server/Spots/MatchQueue.cs` |
| `instance-spot-register` | `Server/Program.cs` |
| `instance-spot-call` | `Client/Program.cs` |
| `actor-contracts` | `Shared/Contracts.cs` |
| `actor-class` | `Server/Actors/Player.cs` |
| `actor-factory` | `Server/Actors/Player.cs` |
| `entry-spot` | `Server/Spots/LobbySpot.cs` |
| `actor-handlers` | `Server/Actors/PlayerHandlers.cs` |
| `actor-push` | `Server/Actors/PlayerHandlers.cs` |
| `actor-register` | `Server/Program.cs` |
| `actor-create-call` | `Client/Program.cs` |
| `actor-message-call` | `Client/Program.cs` |
| `stream-contracts` | `Shared/Contracts.cs` |
| `session-class` | `Server/Sessions/GameSession.cs` |
| `session-handler` | `Server/Sessions/PingHandler.cs` |
| `stream-register` | `Server/Program.cs` |
| `stream-client` | `StreamClient/Program.cs` |
| `session-actor-contracts` | `Shared/Contracts.cs` |
| `session-actor-bind` | `Server/Sessions/AuthenticateHandler.cs` |
| `session-actor-relay` | `Server/Sessions/GameSession.cs` |
| `session-actor-client` | `StreamClient/Program.cs` |
| `single-actor-send` | `StreamClient/Program.cs` |
| `actor-id-receive` | `StreamClient/Program.cs` |
| `actor-handle-per-handle-receive` | `StreamClient/Program.cs` |
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
| `monitoring-call` | `Client/Program.cs` |

## 계약을 확인한 자리

이 코드는 가이드 문서가 아니라 **동작하는 샘플과 공개 계약**을 근거로 썼다. 가이드와
어긋나는 지점은 아래와 같다.

| 내용 | 가이드 표기 |
|---|---|
| node 직접 호출의 packet 이름 | 가이드 05장 §9는 handler에만 `"ops.node.status"`를 준다. 호출하는 쪽은 payload 타입 이름을 쓰므로 그대로 실행하면 `outcome=failed`다. 이름을 정하려면 계약 타입에 `[ZLinkPacket]`을 함께 붙인다 |
| 받는 node의 id 고정 | 가이드 예제에 `SetRoutingId`가 없다. 고정하지 않으면 생성된 id라 부르는 쪽이 지정할 수 없다 |
| Object role에는 Location Store가 필수다 | 06장에 없다 |
| Spot handler는 assembly 자동 스캔으로 등록된다. `Configure()`에서 다시 등록하면 startup에서 거부된다 | 06장이 `AddPacket` 예제를 싣는다 |
| `Objects().Server()`는 MeshNode당 한 번만 호출한다 | 06·07장이 각각 호출한다 |
| Entry Spot의 Actor handler는 `IZLinkEntrySpotActor*Handler`다 | 06장 §5.1 표에 없다 |
| session handler는 자동 스캔되지 않고 packet 이름을 명시해 등록한다 | 09장에 명시가 없다 |
| `Client.Reply`는 Request에만 답한다. Send에는 `Client.Send`로 민다 | 09장에 구분이 없다 |
| Instance Spot factory를 등록하면 Relocation Store도 필수다 | 06장에 없다 |
| Location Store가 있으면 fanout 구독자는 publisher를 자동으로 찾는다. 수동 `Connect`를 함께 쓰면 거부된다 | 05장에 없다 |
