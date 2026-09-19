# Node/TypeScript Tutorial

기능별 가이드가 코드를 읽어 가는 프로그램이다. 이 디렉터리는
[`../../dotnet/tutorial/`](../../dotnet/tutorial/)의 **Channel 메시징과 id로 부르는 Spot
하나**를 옮긴 것이다. Actor·STREAM은 담지 않는다.

## quickstart·샘플과 나눠 두는 이유

| | 목적 |
|---|---|
| [`../quickstart/`](../quickstart/) | 설치부터 첫 응답까지. 기능을 더하지 않는다 |
| **`tutorial/`** (여기) | 기능을 차례로 쌓는다. 기능별 가이드가 이 코드를 읽는다 |
| [`../samples/`](../samples/) | 완결된 업무 흐름을 보이는 application |

## 전제 조건

- **Node.js 22 이상.** `@zlink-systems/zlink`가 `"engines": { "node": ">=22" }`를 선언한다.
  `node --version`으로 확인한다.
- **Docker Desktop(또는 Docker Engine)이 떠 있어야 한다.** Redis 하나를 그 안에서 띄운다(아래
  「실행」). 그 외 설치할 것은 없다.
- **framework 0.18.1부터 Windows에서도 네이티브 빌드 없이 설치된다.** `@zlink-systems/zlink`
  1.2.1은 `prebuilds/linux-x64/`와 `prebuilds/win32-x64/`를 함께 싣는다(#656).
  `@zlink-systems/framework`가 그 버전을 정확히 고정하는 것은 0.18.1부터다 — 그 전 버전을
  고정한 zip에서는 아직 1.2.0만 받아 Windows의 `npm install`이 `node-gyp rebuild`로
  넘어가고, 설치된 Core를 가리키는 `ZLINK_CORE_INSTALL_PREFIX`를 요구하며 실패한다.
  macOS(`darwin-*`)는 아직 prebuild가 없다. 이 문서의 출력은 Windows 11 위 WSL2
  Ubuntu-24.04, Node `v22.23.2`, npm `10.9.8`에서 받은 것이다.

## 내려받기와 설치

이 디렉터리 자체가 이미 받은 것이다 — `zlink-tutorial-node.zip`을 풀면 그 안이 그대로 이
tutorial이다. 저장소를 clone할 필요가 없다. quickstart와 같이 npm registry의 패키지만
참조한다.

```bash
npm install
```

| 패키지 | 고정한 버전 | 근거 |
|---|---|---|
| `@zlink-systems/framework` | `0.18.0` | `npm view @zlink-systems/framework versions` → `0.10.0`부터 `0.18.0`까지 |
| `@zlink-systems/framework-locations-redis` | `0.18.0` | 같은 목록. Spot 단계의 Location Store·Relocation Store 구현이다 |
| `@zlink-systems/nestjs` | `0.18.0` | 같은 목록. `@zlink-systems/framework: '0.18.0'`을 정확히 고정해 의존한다 |
| `@nestjs/common`·`@nestjs/core` | `10.4.22` | `@zlink-systems/nestjs@0.18.0`이 `^10.4.22`를 의존·peer 의존한다 |
| `reflect-metadata` | `0.2.2` | `@zlink-systems/nestjs@0.18.0`의 `^0.2.2` 범위를 만족한다 |
| `@zlink-systems/zlink` | 고정하지 않는다 | `@zlink-systems/framework`가 정확히 고정한다. 0.18.0은 `1.2.0`, 0.18.1부터는 win32-x64 prebuild(#656)가 있는 `1.2.1`이다. 전이 해석에 맡긴다 |

세 `@zlink-systems` 패키지 버전은 저장소의 `scripts/local-package/sync-version.py`가
`framework/languages/node/VERSION`에 맞춰 갱신한다(저장소 안에서만 해당). 손으로 고치지 않는다.

## 빌드

```bash title="linux"
npm run build
```

```powershell title="windows"
npm run build
```

`StreamClient`는 자기 `package.json`을 가진 별도 프로젝트다. connector가 ESM으로만 배포되고
tutorial 본체는 CommonJS이므로 한 tsconfig로 묶지 않는다 — 빌드도 따로 한다(아래 "11. STREAM과
Session-Actor 연결" 참고).

## 실행

Redis가 필요하다(Spot 단계가 쓴다). runner가 따로 없으므로 이 tutorial에서는 직접 하나
띄운다 — 끝나면 직접 정리한다. 터미널을 두 개 열어 두고 따라가는 경우 Server를 먼저
`npm run server`, Client를 `npm run client`로 각각 그대로 실행해도 된다. 아래 블록은 같은
일을 자동으로 하도록 백그라운드로 띄우고 PID를 파일에 남긴다.

```bash title="linux"
docker run -d --rm --name zlink-tutorial-node-redis -p 127.0.0.1:6379:6379 redis:7.2-alpine
sleep 1
npm run server > server.log 2>&1 &
echo $! > server.pid
npm run client > client.log 2>&1 &
echo $! > client.pid
```

```powershell title="windows"
docker run -d --rm --name zlink-tutorial-node-redis -p 127.0.0.1:6379:6379 redis:7.2-alpine
Start-Sleep -Seconds 1
$serverProc = Start-Process -PassThru -NoNewWindow npm.cmd -ArgumentList 'run','server' -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Set-Content -Path server.pid -Value $serverProc.Id
$clientProc = Start-Process -PassThru -NoNewWindow npm.cmd -ArgumentList 'run','client' -RedirectStandardOutput client.log -RedirectStandardError client.err.log
Set-Content -Path client.pid -Value $clientProc.Id
```

전체 기능은 아래 "단계"에서 하나씩 확인한다.

## 검증

Server와 Client가 각각 아래 줄을 찍으면 정상 기동이다.

```
server listening on tcp://0.0.0.0:7701 (mesh "game", routing id "game-server-1")
server admin listening on http://127.0.0.1:5481
```

```
client listening on http://127.0.0.1:5480
```

기동에는 최대 45초까지 걸릴 수 있다(`/mnt/d` 같은 WSL 9p mount 위라면 특히 — 아래
「문제 해결」 참고). 그래서 확인 호출을 고정된 대기 대신 최대 60초 재시도 loop로
넣는다. 성공 여부와 무관하게 항상 두 프로세스와 Redis container를 정리하고, 확인이
끝내 실패했을 때만 0이 아닌 상태로 끝낸다.

```bash title="linux"
ready=""
for _ in $(seq 1 60); do
  if curl -sf http://127.0.0.1:5480/players/p1/profile > response.json 2>/dev/null; then
    ready=1
    break
  fi
  sleep 1
done
[ -n "$ready" ] && cat response.json
kill "$(cat client.pid)" "$(cat server.pid)" 2>/dev/null
docker rm -f zlink-tutorial-node-redis
[ -n "$ready" ]
```

```powershell title="windows"
$ready = $false
for ($i = 0; $i -lt 60; $i++) {
    try {
        $response = Invoke-RestMethod http://127.0.0.1:5480/players/p1/profile -ErrorAction Stop
        $ready = $true
        break
    } catch {
        Start-Sleep -Seconds 1
    }
}
if ($ready) { $response | ConvertTo-Json -Compress }
taskkill /F /T /PID $(Get-Content client.pid) 2>$null
taskkill /F /T /PID $(Get-Content server.pid) 2>$null
docker rm -f zlink-tutorial-node-redis
if (-not $ready) { exit 1 }
```

성공하면 아래 값을 돌려준다.

```json
{"playerId":"p1","nickname":"rookie","level":1}
```

이 값이 오면 mesh·channel 등록과 RouteMesh 호출 경로가 모두 정상이라는 뜻이다. 각 단계의
기대 응답은 아래 "단계"와 "실제 출력"에 있다.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| `docker: Cannot connect to the Docker daemon` | Docker Desktop(또는 dockerd)이 꺼져 있다. 띄운 뒤 다시 실행한다 |
| `curl`이 `Connection refused`를 돌려준다 | Server(`npm run server`)가 아직 뜨지 않았거나 죽었다. 그 터미널의 로그를 먼저 본다 |
| `EADDRINUSE`(포트 충돌) | 아래 「포트」 표의 포트 중 하나를 다른 프로세스가 이미 쓰고 있다. 그 프로세스를 종료하거나 이 tutorial의 다른 실행 중인 인스턴스를 먼저 정리한다 |
| `npm error gyp ERR! ... ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core ... package prefix`(Windows) | 이 zip이 고정한 `@zlink-systems/framework`가 아직 0.18.1 미만이라 `zlink@1.2.0`만 받는다 — win32-x64 prebuild(#656)는 framework 0.18.1(`zlink@1.2.1`)부터다. 그 버전으로 다시 받거나 WSL에서 실행한다 |
| 위와 같은 오류(macOS) | `@zlink-systems/zlink@1.2.1`에도 아직 `darwin-*` prebuild가 없다. Linux(x64)나 Windows(0.18.1부터)에서 실행한다 |
| `EBADENGINE`(Node 버전 경고) | Node.js 22 미만이다. 위 「전제 조건」대로 22 이상으로 올린다 |
| `server listening`이 뜨는 데 20~45초가 걸린다 | 프로젝트가 `/mnt/d` 같은 WSL의 9p mount 위에 있으면 모듈 적재만으로 이만큼 걸린다. Linux 파일 시스템(`~/` 등)으로 옮기면 줄어든다 |

## 포트

| 쓰임 | 값 |
|---|---|
| Client의 HTTP | `127.0.0.1:5480` |
| Server의 admin HTTP | `127.0.0.1:5481` |
| Server의 mesh listen | `0.0.0.0:7701` (advertise `127.0.0.1`) |
| Client의 mesh listen | `0.0.0.0:7702` (advertise `127.0.0.1`) |
| ClientServer channel | `127.0.0.1:7711` |
| Fanout publisher | `127.0.0.1:7712` |
| Server의 stream listen | `0.0.0.0:7721` (WebSocket) |

## 프로젝트

| 자리 | 역할 |
|---|---|
| `Shared/contracts.ts` | 두 쪽이 함께 쓰는 message 계약, packet 이름, mesh·channel 이름 |
| `Server/main.ts` | mesh·channel·node 직접·ClientServer·Fanout 등록. runtime weight 한 자리만 HTTP로 연다 |
| `Server/Channel/` | channel·ClientServer·Fanout handler |
| `Server/Ops/` | node 직접 호출 handler |
| `Server/Spots/game-room.ts` | id로 불리는 Spot 하나와 그 handler 둘 |
| `Server/Spots/match-queue.ts` | 첫 메시지가 만드는 Instance Spot 하나와 그 handler 하나 |
| `Server/Dispatch/` | 모든 handler를 감싸는 filter |
| `Client/main.ts` | HTTP를 받아 mesh·ClientServer·Fanout으로 호출한다 |
| `Client/zlink-error-response.ts` | Framework 예외의 error kind를 HTTP 상태코드와 본문으로 옮긴다 |
| `Server/Actors/player.ts` | id로 불리는 Actor 하나와 그 handler 둘 |
| `Server/Spots/lobby-spot.ts` | 새로 만들어진 player가 처음 들어가는 Entry Spot |
| `Server/Sessions/` | 외부 client 연결 하나를 맡는 session과 그 handler 둘 |
| `StreamClient/` | mesh 밖의 client. framework가 아니라 connector만 참조하는 **별도 프로젝트**다 |

## 단계

### 1. Channel 메시징 — RouteMesh

요청하는 쪽이 node를 고르지 않는다. 채널 이름만 주면 그 채널을 담당하는 node가 받는다.

```bash
curl http://127.0.0.1:5480/players/p1/profile
# {"playerId":"p1","nickname":"rookie","level":1}

curl -X POST http://127.0.0.1:5480/players/p1/logins
# 202. Server 로그에 login recorded: p1
```

두 번째는 응답을 기다리지 않는 단방향 호출이다.

### 2. Channel 메시징 — node 직접 호출

channel을 거치지 않는 경로다. 받는 쪽은 `mesh.addRequestHandler`로 mesh에 바로 등록하고,
부르는 쪽은 node의 routing id를 지정한다. 운영 명령에만 쓴다.

```bash
curl http://127.0.0.1:5480/ops/nodes/game-server-1/status
# {"meshName":"game","channelName":"(none)",
#  "calledBy":"game-388cbafe-f139-4e61-9c0a-361168e3e822","uptime":"64s","processId":3128502}

curl -i http://127.0.0.1:5480/ops/nodes/no-such-node/status
# 404 {"error":"not_found","message":"MeshNode 'game' request failed with result 102 and errno 14."}
# channel 호출과 달리 후보를 고르지 않으므로 그대로 실패한다.
```

`channelName`이 비어 있는 것이 요점이다. channel이 관여하지 않았다는 뜻이다. `calledBy`는
부른 쪽 node의 routing id다. Client는 routing id를 고정하지 않으므로 Framework가 만든
`game-<uuid>` 꼴이 그대로 보인다. 받는 node가 `routingId('game-server-1')`로 id를 고정하는
이유가 이것이다.

### 3. Channel 메시징 — ClientServer

호출 코드는 위와 거의 같다. 다른 것은 **누가 받느냐**다. 부르는 쪽이 연결한 서버가 받는다.

```bash
curl -X POST http://127.0.0.1:5480/players/p1/tickets
# "ticket-p1"
```

### 4. Channel 메시징 — Fanout

보내는 쪽이 받는 node를 모른다. 구독한 node가 모두 받는다.

```bash
curl -X POST http://127.0.0.1:5480/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# 202. Server 로그에 maintenance notice: scheduled maintenance
```

### 5. Filter

`Server/Dispatch/call-log-filter.ts` 하나가 위 네 경로를 모두 감싼다. 위 호출을 차례로 넣으면
Server 로그가 이렇게 된다.

```
LOG [CallLogFilter] dispatch start: GetPlayerProfile
LOG [CallLogFilter] dispatch done: GetPlayerProfile in 1ms
LOG [CallLogFilter] dispatch start: RecordLogin
LOG [RecordLoginHandler] login recorded: p1
LOG [CallLogFilter] dispatch done: RecordLogin in 1ms
LOG [CallLogFilter] dispatch start: IssueSessionTicket
LOG [CallLogFilter] dispatch done: IssueSessionTicket in 1ms
LOG [CallLogFilter] dispatch start: MaintenanceNotice
LOG [MaintenanceNoticeSubscriber] maintenance notice: scheduled maintenance
LOG [CallLogFilter] dispatch done: MaintenanceNotice in 1ms
LOG [CallLogFilter] dispatch start: GetNodeStatus
LOG [CallLogFilter] dispatch done: GetNodeStatus in 0ms
```

Fanout 구독 handler(`MaintenanceNotice`)까지 filter가 감싼다.

### 6. 실행 중 weight 변경

channel weight는 이 node가 도는 동안 바꿀 수 있는 값이다. 0으로 두면 socket은 열려 있고
처리 중인 호출도 끝나지만, 다른 node가 새 호출의 대상으로 이 node를 고르지 않는다. 100이
보통 값이다.

Server는 이 한 자리를 위해 `127.0.0.1:5481`에 HTTP를 연다. body가 없는 호출이므로 새 값은
query string으로 준다.

```bash
curl -X POST 'http://127.0.0.1:5481/admin/channels/profile/weight?value=0'
# {"channel":"profile","weight":0}

curl -X POST 'http://127.0.0.1:5481/admin/channels/profile/weight?value=100'
# {"channel":"profile","weight":100}
```

weight를 0으로 둔 동안 무엇이 멈추는지가 요점이다. `profile` channel을 거치는 1번과 2번만
대상을 찾지 못하고, node 직접 호출·ClientServer·Fanout은 그대로 200과 202를 받는다. 셋은
channel 후보 선택을 거치지 않기 때문이다. 아래 "실제 출력"에 세 벌을 모두 실었다.

실패한 두 호출은 모두 `Unavailable`로 끝나고 503을 받는다. 상태코드를 정하는 것은
`Client/zlink-error-response.ts`다. Framework가 던지는 `ZLinkFrameworkException`의 error
kind를 HTTP 상태코드로 옮기는 자리이고, 이것이 없으면 둘 다 500이 되어 부르는 쪽이 "지금 받을
node가 없다"와 "서버에 결함이 있다"를 구분하지 못한다.

등록하지 않은 channel 이름은 `ZLinkConfigurationException`이 되고, 이 route는 그것을 400으로
돌려준다.

```bash
curl -X POST 'http://127.0.0.1:5481/admin/channels/no-such-channel/weight?value=50'
# 400 {"error":"RouteMesh channel 'no-such-channel' is not registered."}
```

### 7. Spot — id로 부르기

지금까지의 호출은 모두 대상을 이름으로 골랐다. channel 이름을 주면 Framework가 그 channel을
맡은 node 중 하나를 고르고, routing id를 주면 그 node가 답했다. Spot은 다르다. **id 하나를
주면 그 id의 방이 지금 있는 node로 간다.**

```bash
curl -X POST http://127.0.0.1:5480/rooms   -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "9d36f685-7057-42ad-a341-f7ef080df496"

curl -X POST http://127.0.0.1:5480/rooms/9d36f685-7057-42ad-a341-f7ef080df496/chat   -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
# 202

curl http://127.0.0.1:5480/rooms/9d36f685-7057-42ad-a341-f7ef080df496
# {"title":"lobby","chat":["p1: hello"]}
```

id는 Framework가 만든다. 첫 호출은 응답을 기다리지 않는 단방향이고, 두 번째는 방이 만든 답을
받는다. 방은 두 호출 사이에 상태를 들고 있었다.

Node 쪽에서 알아 둘 것은 다음과 같다.

- **Spot 하나를 등록하는 순간 Location Store와 Relocation Store가 모두 필요하다.** 등록
  자체가 조건이라 relocation을 꺼도 Relocation Store를 요구한다.
- **부르는 쪽 client가 channel과 다르다.** channel은 `ZLINK_ROUTE_CLIENT`, Spot 호출은
  `ZLINK_SPOT_OUTBOUND`, 방을 만드는 것은 `ZLINK_SPOT_MANAGER`다. .NET의
  `IZLinkSpotClient`·`IZLinkSpotManager`에 각각 대응한다.
- **Node user Spot은 admit할 actor 타입을 언제나 이름 짓는다.** 이 방은 actor를 받지 않으므로
  기본 타입을 쓰고 join을 모두 거절한다. .NET의 `IZLinkSpot`에는 그 타입 인자가 없다.

### 8. Instance Spot — 첫 메시지가 만드는 큐

만드는 호출이 없다. 그 id로 첫 메시지가 도착하면 Framework가 만들고 같은 메시지를 처리한다.

```bash
curl -X POST http://127.0.0.1:5480/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p1"}'
# {"waiting":1}

curl -X POST http://127.0.0.1:5480/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p2"}'
# {"waiting":2}
```

큐는 넣은 것을 계속 들고 있다. 같은 id로 또 호출하면 숫자가 이어진다. 처음부터 다시 보려면
다른 id를 쓴다.

Node 쪽에서 알아 둘 것은 다음과 같다.

- **Instance Spot은 `ZLinkInstanceSpot`을 구현하고 actor 타입을 이름 짓지 않는다.** 방과 달리
  create·join callback이 없다. handler는 방과 같은 `zlinkSpotPacketHandler` decorator로 큐
  타입과 packet 이름을 적고, Spot과 handler 모두 module의 `providers`에 올린다.
- **부르는 쪽은 `requestToSpot(...)`에 `.instanceSpot(...).inMesh(...)`를 더한다.** 아직 없는
  큐를 어느 mesh에 어떤 stable type으로 만들지 이 두 호출이 정한다. 방을 부를 때는 id만으로
  충분했다.

### 9. Actor — id로 부르는 플레이어

방이 여럿이 함께 쓰는 자리라면 Actor는 개체 하나다. id를 **부르는 쪽이 정하고**, 같은 id로
다시 만들면 있던 것을 돌려준다.

```bash
curl -X POST http://127.0.0.1:5480/players/p7   -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"   — 같은 호출을 다시 하면 "existing"

curl http://127.0.0.1:5480/players/p7
# {"playerId":"p7","nickname":"anonymous"}

curl -X POST http://127.0.0.1:5480/players/p7/nickname   -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
# 202

curl http://127.0.0.1:5480/players/p7
# {"playerId":"p7","nickname":"veteran"}
```

Node 쪽에서 알아 둘 것은 다음과 같다.

- **Actor는 생성자로 만들어지지 않는다.** Framework가 factory를 통해 만들므로 `Player`는
  `Scope.TRANSIENT`로 등록하고, 의존성은 `PlayerFactory`에서 받는다.
- **Entry Spot을 하나 등록해야 한다.** 새로 만들어진 player가 처음 들어가는 자리이고,
  Object Server마다 하나다.
- **handler는 Spot과 Actor를 함께 받는다.** actor id로 보낸 메시지는 그 Actor가 지금 속한
  Spot 안에서 실행된다.

### 10. Location — 위치 조회

Spot과 Actor는 id로만 불렀고, 어디에 있는지는 Framework가 찾았다. 그 기록을 직접 읽는
호출이다.

```bash
curl http://127.0.0.1:5480/locations/rooms/832cf03d-0f75-4c1d-867f-e9b6f84fa247
# {"spotId":"832cf03d-0f75-4c1d-867f-e9b6f84fa247","node":"game-server-1"}

curl http://127.0.0.1:5480/locations/players/p7
# {"actorId":"p7","node":"game-server-1"}

curl -i http://127.0.0.1:5480/locations/players/ghost
# 404
```

조회는 Location Store만 읽고 대상에게는 아무것도 보내지 않는다. 지금 메시지를 받을 수 있는
대상만 답하므로, 만들어지는 중이거나 옮겨 가는 중이면 빈 값이 온다.

### 11. STREAM과 Session-Actor 연결

외부 client가 붙는다. framework가 아니라 connector만 참조한다.

```bash
cd StreamClient
npm install
npm run build
npm start
```

```
connected: true
round trip: 6ms          # STREAM request/reply
bound player: p1         # 연결을 player에 묶는다
pushed: speedy           # player가 그 연결로 밀어 준다
```

`pushed`가 핵심이다. client는 nickname 변경만 보냈고, 응답이 아니라 **player가 스스로 민
알림**을 받았다.

Node 쪽에서 알아 둘 것은 다음과 같다.

- **stream 전송은 WebSocket이다.** 양쪽 endpoint가 `ws://`다. .NET·C++·Java·Kotlin의
  connector는 TCP를 쓰고 `tcp://`를 적는다.
- **connector는 ESM으로만 배포된다.** 그래서 `StreamClient`가 별도 프로젝트이고, 상대 import에
  `.js` 확장자가 붙는다.
- **session handler는 자동 스캔되지 않는다.** `GameSessionFactory.create`에서
  `context.handlers.addHandler(...)`로 등록하고, packet 이름은 `@ZLinkPacket`이 정한다.
- **`client.reply`는 Request에만 답한다.** 기다리는 요청이 없는 client에 밀 때는
  `context.boundSession.send(...)`를 쓴다.

## 실제 출력

```
$ node --version
v22.23.2
$ npm --version
10.9.8
$ npm install

added 44 packages, and audited 45 packages in 2m
$ npm run build

> zlink-tutorial@0.0.0 build
> tsc -p tsconfig.json
$ node dist/Server/main.js
[Nest] ... LOG [NestFactory] Starting Nest application...
[Nest] ... LOG [InstanceLoader] ServerModule dependencies initialized
[Nest] ... LOG [InstanceLoader] DiscoveryModule dependencies initialized
[Nest] ... LOG [InstanceLoader] ZLinkModule dependencies initialized
server listening on tcp://0.0.0.0:7701 (mesh "game", routing id "game-server-1")
server admin listening on http://127.0.0.1:5481
$ node dist/Client/main.js
[Nest] ... LOG [NestFactory] Starting Nest application...
[Nest] ... LOG [InstanceLoader] ClientModule dependencies initialized
[Nest] ... LOG [InstanceLoader] DiscoveryModule dependencies initialized
[Nest] ... LOG [InstanceLoader] ZLinkModule dependencies initialized
client listening on http://127.0.0.1:5480
```

```
$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/profile
{"playerId":"p1","nickname":"rookie","level":1}
HTTP_STATUS:200

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/logins

HTTP_STATUS:202

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/tickets
"ticket-p1"
HTTP_STATUS:200

$ curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"message":"scheduled maintenance"}' \
    -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/notices

HTTP_STATUS:202

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/game-server-1/status
{"meshName":"game","channelName":"(none)","calledBy":"game-1552add6-0517-4bf3-bd6b-a18306bc7ae3","uptime":"42s","processId":3632973}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/no-such-node/status
{"error":"not_found","message":"MeshNode 'game' request failed with result 102 and errno 14."}
HTTP_STATUS:404
```

weight를 0으로 내리고 같은 다섯 호출을 다시 넣은 뒤 100으로 되돌린 것이 아래다. 위 여섯
호출과 같은 실행에서 이어 받았다.

```
$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' \
    'http://127.0.0.1:5481/admin/channels/profile/weight?value=0'
{"channel":"profile","weight":0}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/profile
{"error":"unavailable","message":"MeshNode 'game' request failed with result 102 and errno 0."}
HTTP_STATUS:503

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/logins
{"error":"unavailable","message":"One-way send route is not connected."}
HTTP_STATUS:503

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/tickets
"ticket-p1"
HTTP_STATUS:200

$ curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"message":"scheduled maintenance"}' \
    -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/notices

HTTP_STATUS:202

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/game-server-1/status
{"meshName":"game","channelName":"(none)","calledBy":"game-1552add6-0517-4bf3-bd6b-a18306bc7ae3","uptime":"44s","processId":3632973}
HTTP_STATUS:200

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' \
    'http://127.0.0.1:5481/admin/channels/profile/weight?value=100'
{"channel":"profile","weight":100}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/profile
{"playerId":"p1","nickname":"rookie","level":1}
HTTP_STATUS:200

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/logins

HTTP_STATUS:202

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/tickets
"ticket-p1"
HTTP_STATUS:200

$ curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"message":"scheduled maintenance"}' \
    -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/notices

HTTP_STATUS:202

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/game-server-1/status
{"meshName":"game","channelName":"(none)","calledBy":"game-1552add6-0517-4bf3-bd6b-a18306bc7ae3","uptime":"46s","processId":3632973}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/no-such-node/status
{"error":"not_found","message":"MeshNode 'game' request failed with result 102 and errno 14."}
HTTP_STATUS:404

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' \
    'http://127.0.0.1:5481/admin/channels/no-such-channel/weight?value=50'
{"error":"RouteMesh channel 'no-such-channel' is not registered."}
HTTP_STATUS:400
```

weight가 0인 동안 1번은 `errno 0`으로, 2번은 `One-way send route is not connected.`로 끝난다.
이름이 없는 node를 부른 위의 `errno 14`와 값이 다르다. 3·4·5번은 같은 구간에서 그대로
200·202·200을 받았다.

**weight를 0으로 둔 두 실패는 error kind가 같다** — 둘 다 `Unavailable`이라서 503이다. 후보를
고르는 단계에서 남은 member가 없다는 뜻이고, 송신 경로와 연결은 그대로 있으므로 `NotFound`가
아니다. framework 0.16.0이 request와 one-way를 이 하나로 맞췄다([#498]). 그 전까지 request는
`ProtocolError`라서 400이었다.

이름이 없는 node를 부른 호출만 `NotFound`라서 404다. 후보를 고르지 않고 routing id로 대상을
적는 경로이고, 그 id를 아는 node가 없다는 뜻이기 때문이다. 이 자리들을 500 하나로 뭉뚱그리지
않는 것이 `Client/zlink-error-response.ts`를 둔 이유다.

[#498]: https://github.com/zlink-systems/zlink/issues/498

`calledBy`·`uptime`·`processId`는 실행할 때마다 달라진다. 위 값은 한 번의 실행에서 받은 것이다.

## 문서가 읽는 방식

문서는 코드를 손으로 옮겨 적지 않고 이 파일들에서 구간을 읽는다. 구간은 소스의
`--8<--` 마커가 정한다. 마커 이름은 .NET tutorial과 같다.

| 마커 | 자리 |
|---|---|
| `channel-contracts` | `Shared/contracts.ts` |
| `clientserver-contracts` | `Shared/contracts.ts` |
| `fanout-contracts` | `Shared/contracts.ts` |
| `node-direct-contracts` | `Shared/contracts.ts` |
| `channel-request-handler` | `Server/Channel/get-player-profile-handler.ts` |
| `channel-send-handler` | `Server/Channel/record-login-handler.ts` |
| `clientserver-handler` | `Server/Channel/issue-session-ticket-handler.ts` |
| `fanout-handler` | `Server/Channel/maintenance-notice-subscriber.ts` |
| `node-direct-handler` | `Server/Ops/node-status-handler.ts` |
| `filter-implementation` | `Server/Dispatch/call-log-filter.ts` |
| `filter-register` | `Server/main.ts` |
| `weight-runtime` | `Server/main.ts` |
| `mesh-register` | `Server/main.ts` |
| `channel-register` | `Server/main.ts` |
| `node-direct-register` | `Server/main.ts` |
| `clientserver-register` | `Server/main.ts` |
| `fanout-subscribe` | `Server/main.ts` |
| `channel-client-register` | `Client/main.ts` |
| `clientserver-client-register` | `Client/main.ts` |
| `fanout-publish-register` | `Client/main.ts` |
| `channel-request-call` | `Client/main.ts` |
| `channel-send-call` | `Client/main.ts` |
| `node-direct-call` | `Client/main.ts` |
| `clientserver-call` | `Client/main.ts` |
| `fanout-call` | `Client/main.ts` |
| `spot-contracts` | `Shared/contracts.ts` |
| `spot-class` · `spot-handlers` | `Server/Spots/game-room.ts` |
| `location-store` · `relocation-store` | `Server/main.ts` |
| `object-server` · `spot-register` | `Server/main.ts` |
| `location-store-client` · `spot-client-register` | `Client/main.ts` |
| `spot-create-call` · `spot-message-call` | `Client/main.ts` |
| `spot-send-call` · `spot-request-call` | `Client/main.ts`. `spot-message-call` 안에 나뉘어 있다 |
| `instance-spot-contracts` | `Shared/contracts.ts` |
| `instance-spot-class` · `instance-spot-handler` | `Server/Spots/match-queue.ts` |
| `instance-spot-register` | `Server/main.ts` |
| `instance-spot-call` | `Client/main.ts` |
| `location-find` | `Client/main.ts` |
| `actor-contracts` | `Shared/contracts.ts` |
| `actor-class` · `actor-factory` · `actor-handlers` | `Server/Actors/player.ts` |
| `actor-send-handler` · `actor-request-handler` · `actor-push` | `Server/Actors/player.ts`. `actor-handlers` 안에 나뉘어 있다 |
| `entry-spot` | `Server/Spots/lobby-spot.ts` |
| `actor-register` | `Server/main.ts` |
| `actor-create-call` · `actor-send-call` · `actor-request-call` | `Client/main.ts` |
| `stream-contracts` · `session-actor-contracts` | `Shared/contracts.ts` |
| `session-class` · `session-actor-relay` | `Server/Sessions/game-session.ts` |
| `session-handler` | `Server/Sessions/ping-handler.ts` |
| `session-actor-bind` | `Server/Sessions/authenticate-handler.ts` |
| `stream-register` | `Server/main.ts` |
| `stream-client` · `session-actor-client` | `StreamClient/main.ts` |
| `error-mapping` | `Client/zlink-error-response.ts` |

## .NET tutorial과 표면이 다른 지점

같은 것을 설명하지만 쓰는 이름과 자리가 다르다.

| 내용 | .NET | Node |
|---|---|---|
| 보내는 message 계약 | `record`. 무엇이든 된다 | **`class`여야 한다.** packet 이름을 `payload.constructor.name`에서 뽑고 `'Object'`를 거부하므로, object literal은 쓸 수 없다. 받기만 하는 응답 타입은 `interface`로 둔다 |
| handler 등록 | `AddRequestHandler<THandler, TReq, TRes>()`. 이름은 타입에서 나온다 | `addRequestHandler(packetName, HandlerType)`. packet 이름을 문자열로 준다 |
| handler 생성 | DI 컨테이너가 assembly에서 찾는다 | NestJS `providers`에 직접 올린다. filter도 같다 |
| filter 등록 | `options.UseFilter<CallLogFilter>()` | `builder.options({ filters: [CallLogFilter] })`. 배열 순서가 실행 순서다 |
| routing id 고정 | `SetRoutingId(RoutingId.From("game-server-1"))` | `routingId('game-server-1')`. Node의 `RoutingId`는 `string`의 별칭이다 |
| wildcard bind | `Listen("tcp://0.0.0.0:7201")`만으로 된다 | advertise host를 함께 줘야 한다. 없으면 `ZLinkConfigurationException`으로 startup이 막힌다 |
| node 직접 handler | `mesh.AddRouteRequestHandler<...>()` | `mesh.addRequestHandler(packetName, Type)`. 이름이 channel 쪽과 같고, `mesh.channel(...)`을 거치지 않는 것으로 구분한다 |
| Fanout 구독 handler | `AddHandler<TSub, TMsg>()` | `addPublishHandler(packetName, Type)` |
| ClientServer 호출 | `IZLinkRouteClient.RequestToChannel(...)`가 mesh channel과 ClientServer를 모두 받는다 | **client가 다르다.** mesh channel과 node 직접은 `ZLINK_ROUTE_CLIENT`, ClientServer는 `ZLINK_CHANNEL_CLIENT`다. `ZLinkRouteClient.requestToChannel`은 mesh channel만 찾는다 |
| 호출 종결자 | `.Async<T>(ct)` | `.submit<T>(signal?)`. 동기 종결자는 없다 |
| HTTP surface | ASP.NET Core `app.MapGet(...)` | `node:http`. 이 저장소의 Node 코드가 NestJS HTTP module을 쓰는 자리가 없다(quickstart와 같은 판단) |
| runtime weight 접근 | `IZLinkRouteMeshRuntimeOptions`를 handler 인자로 주입받는다 | `ZLINK_ROUTE_MESH_RUNTIME_OPTIONS` token을 `app.get<ZLinkRouteMeshRuntimeOptions>(..., { strict: false })`로 꺼낸다. provider는 `addRouteMesh`를 한 번이라도 부른 registration에만 등록된다 |
| runtime weight 대입 | `mesh.Channel(channel).Weight = value` | `mesh.channel(channel).weight = value`. 이름만 camelCase로 바뀐다. `channel(...)`이 돌려주는 것은 값을 복사한 객체가 아니라 getter·setter 쌍이므로 대입이 곧 runtime 값의 변경이다 |
| weight 인자 전달 | ASP.NET Core가 `int value`를 query string에서 binding한다 | `url.searchParams.get('value')`를 `Number(...)`로 직접 읽는다. 정수 0..10000을 벗어나면 `ZLinkConfigurationException`이고, 이 tutorial은 그것을 400으로 돌려준다 |
| process 수명 | `app.RunAsync()` | `NestFactory.createApplicationContext(...)`. HTTP listener 없이 DI context만 띄운다 |
| Framework 예외 → HTTP 상태코드 | `IApplicationBuilder.Use(...)` middleware 하나를 endpoint 앞에 등록한다 | **NestJS `ExceptionFilter`를 쓸 수 없다.** filter는 Nest의 request pipeline에서만 돈다. 이 process는 `createApplicationContext`로 뜨므로 HTTP adapter가 없고, `INestApplicationContext`에는 `useGlobalFilters`도 없다. 대신 `withZLinkErrorResponse(...)`가 route dispatch 전체를 한 번 감싼다. 매핑 표와 본문 모양은 .NET과 같다 |

## .NET tutorial과 의미가 달라진 지점

| 내용 | .NET | Node |
|---|---|---|
| Fanout 구독자가 publisher를 찾는 방법 | Location Store가 준다. 수동 `Connect`를 함께 쓰면 거부된다 | `enableSubscriber('tcp://127.0.0.1:7712')`로 endpoint를 직접 준다. Store 없이 도는 것은 이 덕분이다. 인자 없는 `enableSubscriber()`가 Store를 쓰는 쪽이고, 둘을 한 channel에 섞으면 startup이 막힌다 |
| `PeerConnections.Connect(RoutingId, endpoint)` | 이것이 없으면 node 직접 호출이 대상을 모른다며 거부된다 | **거부되지 않는다.** 이 tutorial의 compiled 출력에서 routing id를 뺀 `connect('tcp://127.0.0.1:7701')`로 바꿔 실행해 확인했다. channel 호출도 node 직접 호출도 그대로 200을 받았다. Node에서 routing id를 주는 것은 "그 endpoint에 이 node가 있다"를 기록하는 쪽에 가깝다 |
| Location Store / Relocation Store 등록 | Server·Client 양쪽에 있다(Spot·Actor 때문) | 같다. Spot 단계가 들어오면서 Server에 둘 다, Client에 Location Store를 등록한다 |
| `Objects().Server()` / `Objects().Client()` | Server·Client 양쪽에 있다 | 같다. 부르지 않는 것이 "role 없음"이므로, Spot 이전 단계에서는 이 호출이 없었다 |
| Fanout publisher의 identity | 적지 않아도 된다 | **Location Store를 등록하면 적어야 한다.** Store가 publisher마다 행을 하나 두므로 `setRoutingIdPrefix(...)`나 `routingId(...)` 중 하나가 필요하다. 없으면 startup이 `channel 'broadcast' publisher must select a fixed routing id or an automatic routing id prefix.`로 막힌다. Store가 없던 단계에서는 이 줄도 없었다 |
| Server의 HTTP | 5081에 `weight-runtime` 한 자리를 연다 | 5481에 같은 한 자리를 연다. 포트만 다르다. Client가 5480을 쓰기 때문이다 |
| `uptime` 계산 | `DateTime.Now - Process.GetCurrentProcess().StartTime` | `process.uptime()`. 같은 것을 재지만 Node는 이미 초 단위로 준다 |

## 계약을 확인한 자리

가이드 문서가 아니라 **공개 계약과 동작하는 코드**를 근거로 썼다(저장소 안에서 확인한 것이라
이 zip에는 그 소스가 없다). 읽은 자리는
`framework/languages/node/packages/framework/src/contracts/`,
`framework/languages/node/packages/nestjs/src/`,
`framework/languages/node/e2e/`이다. 대조 결과 어긋나는 것은 아래와 같다.

| 내용 | 문서 표기 |
|---|---|
| `ZLinkRouteClient`에 `sendToSpot`/`requestToSpot`이 있다 | Node interface 명세 02장 §4가 둘을 `ZLinkRouteClient`에 싣고 인자를 `spotId: SpotId`로 적는다. 공개 계약 `contracts/Channels/RouteCalls.ts`의 `ZLinkRouteClient`에는 둘이 없고(`ZLinkSpotClient` 쪽에 있다), 구현 `DefaultZLinkRouteClient`에는 있으나 인자가 `SpotHandle`이다 |
| NestJS builder의 Fanout 구독 topic 지정 | 02장 §1의 `ZLinkFanoutChannelBuilder`에는 `subscribe(topic)`·`connect(endpoint)`·`subscriberConnections()`가 있다. `@zlink-systems/nestjs`의 `ZLinkNestFanoutChannelBuilder`에는 셋 다 없고 `enableSubscriber(endpoint?)`만 있다. topic을 하나도 등록하지 않으면 빈 prefix로 전체를 구독한다 |
| `@zlink-systems/zlink@1.2.0`의 prebuild 범위(framework 0.18.1부터 1.2.1로 해결) | 패키지의 `files`는 `prebuilds/win32-*/*.dll`과 `prebuilds/darwin-*/*.dylib`를 싣도록 적혀 있었다. 1.2.0의 npm tarball에는 `prebuilds/linux-x64/`만 들어 있어 Windows·macOS에서 `npm install`이 source build로 넘어갔다(#656). 1.2.1은 `prebuilds/win32-x64/`를 추가로 담고, `@zlink-systems/framework`는 0.18.1부터 그 버전을 고정한다 — `darwin-*`는 여전히 없다 |
