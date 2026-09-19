# .NET Tutorial

기능별 가이드가 코드를 읽어 가는 프로그램이다. 장을 하나씩 따라가면 이 프로그램이 그
순서대로 커진다.

이 문서는 .NET SDK와 Docker만 있으면 되고 저장소 checkout이 필요 없다. 영어 대응
문서는 [README.md](README.md)다.

## 전제 조건

- **.NET SDK 8.0 이상** — `dotnet --version`이 `8.0.x` 이상 SDK를 보고해야 한다.
- **Docker Desktop**(또는 다른 Docker Engine) — 방·큐·플레이어는 위치를 기록할
  Location Store가 있어야 동작한다. Channel 메시징까지는 필요 없다.

```bash title="linux"
docker run --rm -d -p 6379:6379 --name zlink-tutorial-dotnet-redis redis:7.2-alpine
```

```powershell title="windows"
docker run --rm -d -p 6379:6379 --name zlink-tutorial-dotnet-redis redis:7.2-alpine
```

## 내려받기와 설치

tutorial은 공개된 `Zlink.Framework` NuGet 패키지만 참조한다(저장소 checkout이
전혀 필요 없다). `zlink-tutorial-dotnet.zip`을 아무 디렉터리에나 풀고, 이 문서의
명령을 그 `zlink-tutorial-dotnet` 디렉터리에서 실행한다(저장소 checkout이면 같은
명령을 `framework/languages/dotnet/tutorial`에서 실행한다). 첫 빌드에서 `dotnet
build`가 암묵적으로 실행하는 `dotnet restore`가 그 패키지를 nuget.org에서
내려받는다.

## 빌드

문서가 보여주는 코드와 독자가 nuget.org에서 받는 라이브러리를 같은 것으로 유지한다.
저장소 안에서 빌드해도 `Zlink.Framework` 패키지를 참조하며, 이 디렉터리만 복사해 나가도
그대로 빌드된다.

```bash title="linux"
dotnet build Tutorial.sln
```

```powershell title="windows"
dotnet build Tutorial.sln
```

저장소 소스와 대조해야 할 때만 아래로 바꾼다(저장소 checkout에서만 동작한다).

```bash
dotnet build Tutorial.sln -p:ZLinkTutorialUseLocalSource=true
```

패키지 버전은 [`Directory.Packages.props`](Directory.Packages.props)에 있고
`scripts/local-package/sync-version.py`가 `../VERSION`에 맞춰 갱신한다.

## 실행

Server를 먼저 백그라운드로 띄우고, Client가 그 위에 HTTP를 연다. 둘 다 준비되면
요청 하나로 서로 연결됐는지 확인하고, 그 결과를 다음 절이 읽을 파일에 남긴다.

```bash title="linux"
dotnet run --project Server/Server.csproj > server.log 2>&1 &
echo $! > server.pid
dotnet run --project Client/Client.csproj > client.log 2>&1 &
echo $! > client.pid
for _ in $(seq 1 50); do
  curl -sf http://127.0.0.1:5080/players/warmup/profile >/dev/null 2>&1 && break
  sleep 0.2
done
curl -sf http://127.0.0.1:5080/players/p1/profile | tee tutorial-verify.log
```

```powershell title="windows"
$server = Start-Process dotnet -ArgumentList "run","--project","Server/Server.csproj" `
  -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru -WindowStyle Hidden
$server.Id | Out-File server.pid
$client = Start-Process dotnet -ArgumentList "run","--project","Client/Client.csproj" `
  -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru -WindowStyle Hidden
$client.Id | Out-File client.pid
for ($i = 0; $i -lt 50; $i++) {
  try { Invoke-RestMethod -Uri "http://127.0.0.1:5080/players/warmup/profile" -TimeoutSec 1 | Out-Null; break }
  catch { Start-Sleep -Milliseconds 200 }
}
Invoke-RestMethod -Uri "http://127.0.0.1:5080/players/p1/profile" | ConvertTo-Json -Compress | Tee-Object -FilePath tutorial-verify.log
```

## 검증

`tutorial-verify.log`에 `"playerId":"p1"`이 있으면 Server와 Client가 서로 연결된
것이다("실행" 절이 부른 것과 같은 요청이 낸 응답이다). 확인이 끝나면 두 process를
내린다.

```bash title="linux"
grep -q '"playerId":"p1"' tutorial-verify.log
kill "$(cat client.pid)" "$(cat server.pid)" 2>/dev/null || true
```

```powershell title="windows"
if (-not (Select-String -Path tutorial-verify.log -Pattern '"playerId":"p1"' -Quiet)) {
  throw "tutorial verify failed: $(Get-Content tutorial-verify.log -Raw)"
}
Get-Content client.pid, server.pid | ForEach-Object { Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue }
```

"## 단계" 아래 각 기능의 curl 명령도 각자 적힌 응답을 그대로 내야 그 기능이
동작하는 것이다. 전체를 자동으로 확인하려면
[`.github/workflows/framework-tutorial.yml`](https://github.com/zlink-systems/zlink/blob/main/.github/workflows/framework-tutorial.yml)의
순서를 그대로 따라간다 — 모든 단계를 통과하면 마지막 줄이 `all tutorial steps
passed`다.

## 문제 해결

- **Docker가 실행 중이 아니다 / Redis에 연결할 수 없다** — Docker Desktop(또는
  Docker Engine)을 띄우고 위 "전제 조건"의 `docker run` 명령으로 Redis를 다시
  띄운다.
- **6379 포트가 이미 쓰이고 있다** — 이전 실행의 Redis 컨테이너가 아직 떠 있는지
  `docker ps`로 확인한다. 이 tutorial은 샘플과 달리 고정된 `redis://127.0.0.1:6379`를
  쓰므로 컨테이너를 하나만 띄워 재사용한다(`docker rm -f zlink-tutorial-dotnet-redis`로
  지우고 다시 띄우면 깨끗한 상태로 돌아간다).
- **`dotnet`이 호환되는 SDK가 없다고 한다** — .NET 8.0 이상 SDK를 설치한다.
- **RID 등록이 `RejectedConflict`로 거부된다** — 이전 실행이 남긴 오래된 키가 같은
  Redis에 남아 있을 때 나타난다. 이 tutorial이 쓰는 키만 골라 지운다(다른 곳에
  같은 Redis를 쓰고 있다면 그 데이터는 건드리지 않는다).

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial:*' | xargs -r redis-cli del
  ```

## 프로젝트

| 프로젝트 | 역할 |
|---|---|
| `Shared` | 두 쪽이 함께 쓰는 message 계약 |
| `Server` | channel handler, 방·큐, 플레이어, client session을 실행한다 |
| `Client` | HTTP를 받아 mesh로 호출한다. 방과 플레이어를 만들고 부른다 |
| `StreamClient` | 외부 TCP client. framework 없이 connector만 참조한다 |

## quickstart·샘플과 나눠 두는 이유

| | 목적 |
|---|---|
| [`../quickstart/`](../quickstart/) | 설치부터 첫 응답까지. 기능을 더하지 않는다 |
| **`tutorial/`** (여기) | 기능을 차례로 쌓는다. 기능별 가이드가 이 코드를 읽는다 |
| [`../samples/`](../samples/) | 완결된 업무 흐름을 보이는 application |

tutorial은 기능마다 최소한만 담는다. 도메인 로직을 넣기 시작하면 샘플의 축소판이 된다.

## 단계

각 기능은 따로 읽어도 된다. 앞 단계를 하지 않아도 그다음 단계가 동작한다.

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

`channelName`이 비어 있는 것이 요점이다. channel이 관여하지 않았다는 뜻이다. `calledBy`는
부른 쪽 node의 routing id이고, 나머지 값은 답한 process 하나의 것이다.

이 호출에는 등록 쪽 조건이 하나 있다. 받는 node가 `SetRoutingId`로 id를 고정해야 한다. 고정하지
않으면 Framework가 만든 id가 붙어 부르는 쪽이 URL에 적을 수 없다.

부르는 쪽은 그 node와 peer로 연결되어 있기만 하면 된다. `Connect(RoutingId, endpoint)`로 기대하는
id를 함께 적을 수 있는데, 연결을 그 node 하나로 묶는 것이지 node 직접 호출의 조건은 아니다.

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

방을 만들고 id를 받는다. 이후로는 그 id만으로 부른다.

```bash
ROOM=$(curl -s -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"bronze-1"}' | tr -d '"')

curl -X POST http://127.0.0.1:5080/rooms/$ROOM/chat \
  -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'

curl http://127.0.0.1:5080/rooms/$ROOM
# {"title":"bronze-1","chat":["p1: hello"]}
```

호출 두 번 사이에 방이 상태를 들고 있다.

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

큐는 넣은 것을 계속 들고 있다. 같은 id로 또 호출하면 숫자가 이어진다. 처음부터 다시 보려면
다른 id를 쓴다.

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
bound player: p1          # 연결을 player에 묶는다
pushed: speedy            # player가 그 연결로 밀어 준다
```

`pushed`가 핵심이다. client는 nickname 변경만 보냈고, 응답이 아니라 **player가 스스로 민 알림**을
받았다.

### 9. 모니터링

```bash
curl http://127.0.0.1:5080/status
# {"state":"Serving","ready":true,"acceptingWork":true,"safeToShutdown":true,...}
```

## 문서가 읽는 방식

문서는 코드를 손으로 옮겨 적지 않고 이 파일들에서 구간을 읽는다. 구간은 소스의
`--8<--` 마커가 정한다.

```
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
```

마커 이름을 바꾸면 그 구간을 읽는 문서가 조용히 빈 코드 블록을 낸다. 이름을 바꿀 때는
문서를 함께 고친다. CI가 아래 표와 소스를 대조한다.

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
