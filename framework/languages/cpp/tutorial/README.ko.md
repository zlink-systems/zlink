[English](./README.md) | [한국어](./README.ko.md)

# C++ Tutorial

기능별 가이드가 코드를 읽어 가는 프로그램이다. 장을 하나씩 따라가면 이 프로그램이 그
순서대로 커진다. `.NET Tutorial`을 C++로 옮긴 것이며, Channel 메시징 네 가지(RouteMesh
요청·단방향, node 직접 호출, ClientServer, Fanout), handler filter, runtime weight 변경, Spot,
Actor, Location, STREAM, HTTP client를 담는다.

이 디렉터리는 `zlink-cpp-examples` 저장소의 `tutorial/`이다. 아래 절차는 GitHub Release에
공개된 Core·binding·framework 패키지와 Conan을 사용한다.

| | 목적 |
|---|---|
| quickstart (저장소 `framework/languages/cpp/quickstart/`) | 설치부터 첫 응답까지. 기능을 더하지 않는다 |
| **tutorial** (여기) | 기능을 차례로 쌓는다. 기능별 가이드가 이 코드를 읽는다 |
| samples (`zlink-cpp-examples` 저장소의 `samples/`) | 완결된 업무 흐름을 보이는 application |

## 차례

- [전제 조건](#전제-조건)
- [내려받기와 설치](#내려받기와-설치)
- [빌드](#빌드)
- [실행](#실행)
- [검증](#검증)
- [문제 해결](#문제-해결)
- [프로젝트 구성](#프로젝트-구성)
- [단계별 확인](#단계별-확인)
- [문서가 읽는 방식](#문서가-읽는-방식)
- [.NET tutorial과 달라진 지점](#net-tutorial과-달라진-지점)

`빌드`·`실행`·`검증` 절의 명령 블록은 `title="linux"`(bash)와 `title="windows"`(PowerShell)로
표시되어 있다. 각 블록은 `zlink-cpp-examples` 저장소를 clone한 뒤 `tutorial/`에서 그대로
실행되며, 릴리스 CI가 같은 블록을 그대로 돌린다.

## 전제 조건

| 도구 | Windows | Linux · WSL |
|---|---|---|
| C++20 컴파일러 | Visual Studio 2022 17.4 이상, **Desktop development with C++** 워크로드 (MSVC 19.44로 확인) | GCC 13 이상 (13.3으로 확인) |
| CMake | 3.24 이상 (Visual Studio가 설치하는 3.31로 확인) | 3.24 이상 (3.28로 확인) |
| Ninja | 필요 없음 | 권장. 없으면 Makefile로 빌드한다 |
| Conan 2 | `pipx install conan` (`py -m pip install --user conan`도 가능) | `pipx install conan` (`python3 -m pip install --user conan`도 가능) |
| Docker Desktop | Redis 하나를 컨테이너로 띄운다. 설치되어 실행 중이어야 한다 | 같다 (WSL integration 또는 Linux의 Docker Engine) |

이 밖에는 아무것도 필요 없다. zlink 저장소, Node.js, 시스템 패키지의 Boost는 쓰지 않는다.
Conan은 pipx(또는 pip)로 설치하며 ConanCenter의 세 번째 파티 바이너리를 받는다. 지원 컴파일러에서
**첫 설치는 약 3분**이며, 맞는 바이너리가 없는 패키지만 Conan이 소스에서 빌드한다. 두 번째부터는
로컬 캐시를 재사용한다.

## 내려받기와 설치

[`zlink-cpp-examples`](https://github.com/zlink-systems/zlink-cpp-examples) 저장소를 clone한다.
아래 명령은 모두 그 저장소의 `tutorial/` 안에서 실행한다.

설치는 `bootstrap.cmake` 하나가 한다 — [빌드](#빌드) 블록의 첫 줄이다. 이 스크립트는 GitHub
Release에서 세 아카이브 — 이 플랫폼의 Core prebuilt(`core/v1.2.0`), C++ binding
소스(`cpp/v1.2.0`), framework 소스(`framework-cpp/v0.18.0`) — 를 받아 binding과 framework를
빌드해 `.zlink/install/`에 설치하고, 이 프로젝트를 `build/`에 구성한다. framework 버전만
스크립트에 적혀 있고, Core·binding 버전과 세 번째 파티 목록은 framework 아카이브가 정한다.
기본 package manager는 Conan이며 vcpkg fallback은 `-DZLINK_PACKAGE_MANAGER=vcpkg`로 고른다.
두 번째 실행부터는 받은 것과 지은 것을 그대로 쓴다.

병렬도는 논리 코어 수가 기본이며 `cmake -DZLINK_JOBS=4 -P bootstrap.cmake`처럼 `-P` 앞에
두어 줄인다. 다시 처음부터 하려면 `.zlink/`와 `build/`를 지운다.

## 빌드

```bash title="linux"
cmake -P bootstrap.cmake
cmake --build build --parallel
```

```powershell title="windows"
cmake -P bootstrap.cmake
cmake --build build --config Release --parallel
```

실행 파일 셋이 나온다 — Windows는 `build\Release\`, Linux는 `build/` 아래다. Windows에서는
Core `zlink.dll`과 세 번째 파티 DLL이 실행 파일 옆에 함께 복사된다(Windows에는 RPATH가
없어 loader가 실행 파일 옆만 본다).

## 실행

Redis가 `127.0.0.1:6379`에 있어야 한다. Spot·Actor·Location 단계가 Location Store로 쓴다.
아래 블록은 Redis를 Docker로 띄우고 Server, Client를 차례로 띄운 뒤 첫 요청으로 두 process가
mesh로 연결됐는지 확인한다. handler와 filter의 로그는 **stderr**로 나간다.

```bash title="linux"
docker run -d --rm --name zlink-tutorial-redis -p 127.0.0.1:6379:6379 redis:7-alpine && until docker exec zlink-tutorial-redis redis-cli ping 2>/dev/null | grep -q PONG; do sleep 0.2; done
./build/tutorial_server > server.log 2>&1 &
./build/tutorial_client > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5180/players/p1/profile > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:5180/players/p1/profile
```

```powershell title="windows"
docker run -d --rm --name zlink-tutorial-redis -p 127.0.0.1:6379:6379 redis:7-alpine | Out-Null; if ($LASTEXITCODE -eq 0) { while (-not ((docker exec zlink-tutorial-redis redis-cli ping 2>$null) -match 'PONG')) { Start-Sleep -Milliseconds 200 } }
Start-Process -NoNewWindow .\build\Release\tutorial_server.exe -RedirectStandardOutput server.out -RedirectStandardError server.log
Start-Process -NoNewWindow .\build\Release\tutorial_client.exe -RedirectStandardOutput client.out -RedirectStandardError client.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5180/players/p1/profile; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'tutorial-http did not come up' }
$answer
```

PowerShell의 `curl`은 `Invoke-WebRequest`의 별칭이므로 `curl.exe`를 쓰고, JSON 본문의
큰따옴표는 `\"`로 escape한다. 예를 들어 방을 여는 요청은 다음과 같다.

```powershell
curl.exe -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{\"title\":\"lobby\"}'
```

```bash
curl -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{"title":"lobby"}'
```

STREAM 단계의 외부 client는 세 번째 실행 파일이다. HTTP client 단계의 외부 client는 네 번째
실행 파일이다. 두 프로그램은 Server와 Client가 떠 있는 상태에서 자기 검증을 마치고 종료한다.

정리는 process 둘과 Redis 컨테이너를 내리는 것이다.

```powershell
Stop-Process -Name tutorial_server,tutorial_client
docker stop zlink-tutorial-redis
```

```bash
pkill -f build/tutorial_server; pkill -f build/tutorial_client
docker stop zlink-tutorial-redis
```

쓰는 port는 아래와 같다. 다른 언어의 tutorial과 같은 기계에서 함께 돌릴 수 있도록
.NET tutorial과 다른 값을 쓴다.

| 용도 | port |
|---|---|
| Client HTTP | 5180 |
| Server HTTP (운영 endpoint) | 5181 |
| Server mesh | 7401 |
| Client mesh | 7402 |
| ClientServer channel | 7411 |
| Fanout publisher | 7412 |
| Server stream node | 7421 |

## 검증

| 단계 | 성공의 증거 |
|---|---|
| `cmake -P bootstrap.cmake` | 마지막 줄 `-- bootstrap done. Next: cmake --build ...`. `.zlink/install/lib/cmake/zlink_framework/zlink_frameworkConfig.cmake`가 있다 |
| 빌드 | `tutorial_server`·`tutorial_client`·`tutorial_stream_client`·`tutorial_http_client` 네 실행 파일이 있다 |
| 첫 요청 | `curl http://127.0.0.1:5180/players/p1/profile`이 `{"level":1,"nickname":"rookie","playerId":"p1"}`를 낸다 |
| Spot (Redis) | 방을 여는 요청이 방 id 문자열(`"9e78fd70-…"`)을 낸다 |
| Instance Spot | 같은 대기열 id로 두 번 요청하면 `waiting`이 1, 2로 이어진다 |
| STREAM | `tutorial_stream_client`가 `connected: true` … `pushed: speedy` 네 줄을 찍고 0으로 종료한다 |

아래 블록은 [실행](#실행) 블록이 띄운 상태에서 첫 요청의 응답과 STREAM client의 종료 코드로
이를 확인한다.

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5180/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
./build/tutorial_stream_client
echo "tutorial-stream=ok"
```

```powershell title="windows"
if ((curl.exe -s http://127.0.0.1:5180/players/p1/profile) -notmatch '"playerId":"p1"') { throw 'tutorial-http failed' }
Write-Output 'tutorial-http=ok'
& .\build\Release\tutorial_stream_client.exe
if ($LASTEXITCODE -ne 0) { throw 'tutorial-stream failed' }
Write-Output 'tutorial-stream=ok'
```

[단계별 확인](#단계별-확인)에 열한 단계의 요청과 기대 출력이 전부 있다.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| `bootstrap: could not find Conan` | `pipx install conan`(또는 `python3 -m pip install --user conan`)으로 Conan 2를 설치하고 실행 파일 경로를 `PATH`에 넣는다 |
| `ERROR: Invalid setting ...` | 선택한 컴파일러가 ConanCenter의 지원 바이너리 구성과 다르다. 표의 컴파일러 버전을 쓰거나 `-DZLINK_PACKAGE_MANAGER=vcpkg`를 지정한다 |
| `bootstrap: download failed: https://github.com/...` | GitHub Release에 닿지 못했다. 프록시·방화벽을 확인한다. 다시 실행하면 처음부터 다시 받는다 |
| `CMake Error ... No CMAKE_CXX_COMPILER could be found` / `Visual Studio 17 2022 could not find any instance` | 컴파일러가 없다. Windows는 **Desktop development with C++** 워크로드, Linux는 `g++`를 설치한다 |
| `error LNK2038: mismatch detected for 'RuntimeLibrary'` | `.zlink/`가 다른 옵션으로 빌드된 잔재다. `.zlink/build`·`.zlink/cpp`·`.zlink/install`·`build`를 지우고 bootstrap부터 다시 한다 |
| Windows에서 실행 파일이 아무 출력 없이 즉시 끝난다 (종료 코드 `-1073741515`, `STATUS_DLL_NOT_FOUND`) | `zlink.dll`이 실행 파일 옆에 없다. `cmake --build build --config Release`를 다시 실행하면 post-build 단계가 `build\Release\`에 복사한다 |
| `docker: error during connect` / `Cannot connect to the Docker daemon` | Docker Desktop이 실행 중이 아니다. 띄운 뒤 `docker run ...`을 다시 한다 |
| `docker: Error response from daemon: ... port is already allocated` / `Bind for 127.0.0.1:6379 failed` | 6379를 다른 Redis가 쓰고 있다. 그 Redis를 그대로 써도 된다 — tutorial은 `127.0.0.1:6379`만 본다 |
| Server 로그에 `Location Store` 연결 실패 | Redis가 없다. Channel 단계까지는 그대로 돌지만 Spot·Actor·Location 단계는 실패한다 |
| `bind: Address already in use` / `Only one usage of each socket address` | 위 표의 port를 다른 process가 쓴다. 이전 실행의 `tutorial_server`·`tutorial_client`가 남아 있는지 확인한다 |
| `curl: (7) Failed to connect to 127.0.0.1 port 5180` | Client가 아직 뜨지 않았거나 죽었다. Client의 stderr를 본다 |

## 프로젝트 구성

| 프로젝트 | 역할 |
|---|---|
| `Shared` | 두 쪽이 함께 쓰는 message 계약 |
| `Server` | channel handler와 node 직접 호출 handler를 실행하고, filter를 건다. 운영 endpoint 하나를 위해 HTTP도 연다 |
| `Client` | HTTP를 받아 mesh로 호출한다 |
| `StreamClient` | mesh 밖의 client. framework가 아니라 connector만 링크한다 |
| `HttpClient` | mesh 밖의 client. framework가 아니라 http-client package만 링크한다 |
| `bootstrap.cmake` | 공개 아카이브로 framework를 설치하고 이 프로젝트를 구성한다 |

`CMakeLists.txt`는 `find_package(zlink_framework CONFIG REQUIRED)`와
`find_package(zlink_http_client_cpp CONFIG REQUIRED)`로 네 실행 파일을 만든다. 자기 프로젝트에
옮길 때는 `CMAKE_PREFIX_PATH`에 `.zlink/install`을 주면 된다.

## 단계별 확인

각 기능은 따로 읽어도 된다. 앞 단계를 하지 않아도 그다음 단계가 동작한다.
아래 출력은 모두 실제로 찍어 본 것이다.

### 1. Channel 메시징 — RouteMesh

요청하는 쪽이 node를 고르지 않는다. 채널 이름만 주면 그 채널을 담당하는 node가 받는다.

```console
$ curl http://127.0.0.1:5180/players/p1/profile
{"level":1,"nickname":"rookie","playerId":"p1"}

$ curl -i -X POST http://127.0.0.1:5180/players/p1/logins
HTTP/1.1 202 Accepted
Content-Length: 0
```

두 번째는 응답을 기다리지 않는 단방향 호출이다. Server의 stderr에 이렇게 남는다.

```
info class call_log_filter_t - dispatch start: RecordLogin
info class record_login_handler_t - login recorded: p1
info class call_log_filter_t - dispatch done: RecordLogin in 2ms
```

JSON key의 순서는 알파벳 순이다. nlohmann JSON이 object를 정렬된 map으로 들고 있기
때문이며, 선언 순서와 무관하다.

### 2. Channel 메시징 — node 직접 호출

channel을 거치지 않는 경로다. 받는 쪽은 `mesh.add_route_request_handler`로 mesh에 바로
등록하고, 부르는 쪽은 node의 routing id를 지정한다. 운영 명령에만 쓴다.

```console
$ curl http://127.0.0.1:5180/ops/nodes/game-server-1/status
{"calledBy":"game-client-1","channelName":"","meshName":"game","uptime":"10s"}

$ curl -i http://127.0.0.1:5180/ops/nodes/no-such-node/status
HTTP/1.1 404 Not Found
{"correlationId":"http-6","error":"not_found","message":"MeshNode request target was not found"}
```

`channelName`이 비어 있는 것이 요점이다. channel이 관여하지 않았다는 뜻이다. `calledBy`는
부른 쪽 node의 routing id이고, `uptime`은 답한 process 하나의 것이다. channel 호출과 달리
후보를 고르지 않으므로, 없는 node를 적으면 그대로 실패한다.

이 호출에는 등록 쪽 조건이 둘 더 있다. 받는 node가 `set_routing_id`로 id를 고정해야 하고
(고정하지 않으면 생성된 id라 부르는 쪽이 적을 수 없다), 부르는 쪽이
`peer_connections().connect(routing_id, endpoint)`로 어느 id가 그 endpoint에 있는지 알려야
한다. `connect(endpoint)`만 쓰면 channel 호출은 되지만 node 직접 호출은 대상을 모른다.

handler가 `route_message_context_t`를 함께 받아야 위 세 값이 손에 들어온다. 인자를 하나만
선언한 handler도 유효하며, 그때는 context가 오지 않는다.

### 3. Channel 메시징 — ClientServer

호출 코드는 위와 같다. 다른 것은 **누가 받느냐**다. 부르는 쪽이 연결한 서버가 받는다.

```console
$ curl -X POST http://127.0.0.1:5180/players/p1/tickets
"ticket-p1"
```

C++에서는 부르는 쪽이 쓰는 타입도 다르다. RouteMesh는 `route_client_t`, ClientServer는
`channel_client_t`다. `route_client_t::request_to_channel`에 ClientServer channel 이름을 주면
호출이 이렇게 거절된다.

```
503 {"error":"unavailable","message":"RouteMesh channel 'ticketing' is not registered"}
```

### 4. Channel 메시징 — Fanout

보내는 쪽이 받는 node를 모른다. 구독한 node가 모두 받는다.

```console
$ curl -i -X POST http://127.0.0.1:5180/notices \
    -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
HTTP/1.1 202 Accepted
Content-Length: 0
```

Server의 stderr:

```
info class call_log_filter_t - dispatch start: MaintenanceNotice
info class maintenance_notice_subscriber_t - maintenance notice: scheduled maintenance
info class call_log_filter_t - dispatch done: MaintenanceNotice in 1ms
```

fanout handler는 다른 handler와 등록 경로가 다르다. 이름 붙인 handler group에 넣고, channel이
그 group을 집는다. `add_fanout_channel(...).add_handler<...>()` 같은 직접 등록은 없다.

```cpp
options.handlers ().group ("broadcast").add_publish<maintenance_notice_subscriber_t> ();
options.add_fanout_channel ("broadcast")
  .connect ("tcp://127.0.0.1:7412")
  .use_handler_group ("broadcast");
```

`connect(...)`가 이 구독자를 수동으로 만든다. Location Store로 publisher를 찾는
`enable_subscriber()`를 옆에 같이 쓰면 startup에서 거절된다.

```
fanout channel 'broadcast' cannot combine automatic discovery with manual subscriber endpoints
```

### 5. Handler filter

위 네 호출의 Server 로그에 한 쌍씩 찍힌 `dispatch start` / `dispatch done`이 filter다.
handler마다 같은 로그를 적지 않아도 되도록 dispatch를 감싼다.

```
info class call_log_filter_t - dispatch start: GetPlayerProfile
info class call_log_filter_t - dispatch done: GetPlayerProfile in 1ms
info class call_log_filter_t - dispatch start: RecordLogin
info class record_login_handler_t - login recorded: p1
info class call_log_filter_t - dispatch done: RecordLogin in 2ms
info class call_log_filter_t - dispatch start: IssueSessionTicket
info class call_log_filter_t - dispatch done: IssueSessionTicket in 1ms
info class call_log_filter_t - dispatch start: MaintenanceNotice
info class maintenance_notice_subscriber_t - maintenance notice: scheduled maintenance
info class call_log_filter_t - dispatch done: MaintenanceNotice in 1ms
info class call_log_filter_t - dispatch start: GetNodeStatus
info class call_log_filter_t - dispatch done: GetNodeStatus in 0ms
```

Channel 요청·단방향, ClientServer, Fanout, node 직접 호출 네 경로 모두 filter를 지난다.

로그의 category가 `class call_log_filter_t`인 것은 `logger_t<T>`의 기본 category가
`typeid(T).name()`이기 때문이다. MSVC의 그 값이 `class `로 시작한다.

### 6. Runtime weight 변경

지금까지의 값은 모두 startup에 정해졌다. weight는 다르다. process를 돌린 채로 바꿀 수 있는
유일한 값이다. 0으로 두면 socket은 열린 채 남고 처리 중인 호출도 끝까지 가지만, 다른 node가
새 호출의 대상으로 이 node를 고르지 않는다. 평상시 값은 100이다.

바꾸는 창구는 `route_mesh_runtime_options_t`다. builder가 아니라 **돌고 있는 mesh**를 가리키는
타입이고, HTTP handler가 생성자로 받는다.

```cpp
explicit channel_weight_handler_t (fw::route_mesh_runtime_options_t &mesh) : _mesh (mesh) {}
```

앞의 다섯 호출과 달리 이 endpoint는 Server가 직접 받는다. Server는 여기서 처음으로 HTTP를
열며, port는 Client의 5180과 겹치지 않게 5181을 쓴다.

```console
$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=0'
HTTP/1.1 200 OK
Content-Type: application/json
X-Correlation-Id: http-1
Content-Length: 32

{"channel":"profile","weight":0}

$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=100'
HTTP/1.1 200 OK
Content-Type: application/json
X-Correlation-Id: http-2
Content-Length: 34

{"channel":"profile","weight":100}
```

응답의 `weight`는 요청 값을 그대로 돌려준 것이 아니라 `weight(value)` 뒤에 `weight()`로 다시
읽은 값이다. 쓰기가 실제로 반영되었다는 뜻이다.

weight가 0인 동안 `profile` channel을 부르는 두 호출은 실패한다. 이 tutorial에는 그 channel을
담당하는 node가 하나뿐이고, 그 하나가 후보에서 빠지면 고를 대상이 남지 않는다. 요청과 단방향
호출의 응답이 서로 다르다.

```console
$ curl -i http://127.0.0.1:5180/players/p1/profile
HTTP/1.1 503 Service Unavailable
{"correlationId":"http-7","error":"unavailable","message":"RouteMesh channel request was not submitted"}

$ curl -i -X POST http://127.0.0.1:5180/players/p1/logins
HTTP/1.1 404 Not Found
{"correlationId":"http-8","error":"not_found","message":"RouteMesh channel send target was not found"}
```

Server의 stderr에는 이 두 호출의 `dispatch start`가 찍히지 않는다. 부르는 쪽이 대상을 고르는
단계에서 멈추므로 message가 Server에 닿지 않는다.

나머지 세 호출은 weight가 0이어도 그대로 답한다. ClientServer(`/players/p1/tickets`)와
fanout(`/notices`)은 RouteMesh channel이 아니고, node 직접 호출(`/ops/nodes/...`)은 후보를
고르지 않고 routing id로 대상을 적기 때문이다.

위 블록의 두 번째 명령으로 weight를 100으로 되돌리면 두 호출 모두 원래대로 답한다.

```console
$ curl http://127.0.0.1:5180/players/p1/profile
{"level":1,"nickname":"rookie","playerId":"p1"}

$ curl -i -X POST http://127.0.0.1:5180/players/p1/logins
HTTP/1.1 202 Accepted
Content-Length: 0
```

위 출력은 framework 0.16.0에서 찍은 것이다. 0.15.0 전까지 C++은 이 경우에도 200을 냈고, 다른 네
언어만 실패했다. weight를 바꾼 node가 새 descriptor를 자기 topology에만 반영하고 peer에게
보내지 않아 부르는 쪽이 옛 weight를 들고 있었던 것으로, 0.15.0이 이 값을 peer에게 함께
보내면서 고쳐졌다(zlink `92c7773`, issue #481).

!!! warning "두 응답의 error kind가 서로 다른 것은 C++만 남은 차이다"

    [#498](https://github.com/zlink-systems/zlink/issues/498)은 후보가 비었을 때 request와
    one-way send가 **둘 다 `Unavailable`**로 끝나도록 맞췄고, 0.16.0에 들어갔다. .NET·Java·
    Kotlin·Node는 이 단계에서 두 호출 모두 503을 낸다. **C++만 one-way send가 아직
    `NotFound`(404)다.**

    request 경로와 달리 RouteMesh channel의 one-way send는
    `framework/src/runtime/host/app.cpp`의 `one_way_native_submit_result`를 지나는데, #498이
    고친 자리는 `channel_outbound_exchange.cpp`의 두 곳이라 이 경로가 빠졌다. 위 404는 그것을
    실행해 확인한 값이다.

거절되는 경우는 다음과 같다.

```console
$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight'
HTTP/1.1 400 Bad Request
{"error":"value is required"}

$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/nope/weight?value=100'
HTTP/1.1 400 Bad Request
{"correlationId":"http-4","error":"protocol_error","message":"RouteMesh channel is not configured: nope"}

$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=99999'
HTTP/1.1 400 Bad Request
{"correlationId":"http-5","error":"protocol_error","message":"channel weight must be in range 0..10000"}
```

첫 번째는 handler가 직접 낸 응답이다. C++에는 query string을 `int` 인자로 묶어 주는 model
binding이 없어 `query_values`에서 손으로 꺼내며, 없는 경우를 handler가 답하지 않으면 500
`invalid map<K, T> key`가 나간다. 뒤의 둘은 runtime이 낸 것이다. 등록하지 않은 channel 이름과
범위 밖의 값은 `route_mesh_runtime_options_t`가 받아 주지 않는다.

이 호출에는 Server의 stderr에 `dispatch start` / `dispatch done`이 찍히지 않는다. filter가 감싸는
것은 이 node가 **받은 message**의 dispatch이고, 이 endpoint는 message가 아니라 이 process 안의
HTTP 호출이기 때문이다.

### 7. Spot — id로 부르기

지금까지의 호출은 모두 대상을 이름으로 골랐다. channel 이름을 주면 Framework가 그 channel을
맡은 node 중 하나를 고르고, routing id를 주면 그 node가 답했다. Spot은 다르다. **id 하나를
주면 그 id의 방이 지금 있는 node로 간다.**

방을 먼저 연다. 응답은 그 방의 id다.

```console
$ curl -X POST http://127.0.0.1:5180/rooms     -H 'Content-Type: application/json' -d '{"title":"lobby"}'
"9e78fd70-edee-47b4-8433-d1539862917f"
```

id는 Framework가 만든다. 그 뒤로는 이 값 하나로 방을 부른다.

```console
$ curl -i -X POST http://127.0.0.1:5180/rooms/9e78fd70-edee-47b4-8433-d1539862917f/chat     -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
HTTP/1.1 202 Accepted

$ curl http://127.0.0.1:5180/rooms/9e78fd70-edee-47b4-8433-d1539862917f
{"chat":["p1: hello"],"title":"lobby"}
```

첫 호출은 응답을 기다리지 않는 단방향이고, 두 번째는 방이 만든 답을 받는다. 방은 두 호출
사이에 상태를 들고 있었다.

C++ 쪽에서 알아 둘 것은 다음과 같다.

- **Spot 하나를 등록하는 순간 Location Store와 Relocation Store가 모두 필요하다.** 등록
  자체가 조건이라 relocation을 꺼도 Relocation Store를 요구한다.
- **mesh node의 Object role을 `none`으로 두지 않는다.** 앞 단계까지는 `none`이었다. Spot을
  등록하려면 Server, 부르려면 Client여야 한다.
- **C++ user Spot은 admit할 actor 타입을 언제나 이름 짓는다.** 이 방은 actor를 받지 않으므로
  기반 타입을 적고 join을 모두 거절한다. .NET의 `IZLinkSpot`에는 그 타입 인자가 없다.

```cpp
class game_room_t : public fw::spot_t<fw::actor_t>
```

### 8. Instance Spot — 첫 메시지가 만드는 대기열

만드는 호출이 없다. 그 id로 첫 메시지가 도착하면 Framework가 만들고 같은 메시지를 처리한다.

```bash
curl -X POST http://127.0.0.1:5180/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p1"}'
# {"waiting":1}

curl -X POST http://127.0.0.1:5180/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p2"}'
# {"waiting":2}
```

대기열은 넣은 값을 계속 들고 있다. 같은 id로 다시 호출하면 숫자가 이어진다. 처음부터 다시
보려면 다른 id를 쓴다.

### 9. Actor — id로 부르는 플레이어

방이 여럿이 함께 쓰는 자리라면 Actor는 개체 하나다. id를 **부르는 쪽이 정하고**, 같은 id로
다시 만들면 있던 것을 돌려준다.

```console
$ curl -X POST http://127.0.0.1:5180/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
"created"

$ curl -X POST http://127.0.0.1:5180/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
"existing"

$ curl http://127.0.0.1:5180/players/p7
{"nickname":"anonymous","playerId":"p7"}

$ curl -i -X POST http://127.0.0.1:5180/players/p7/nickname -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
HTTP/1.1 202 Accepted

$ curl http://127.0.0.1:5180/players/p7
{"nickname":"veteran","playerId":"p7"}
```

C++ 쪽에서 알아 둘 것은 다음과 같다.

- **Actor는 생성자로 만들어지지 않는다.** `player_factory_t`가 만들고 context를 심는다.
- **Entry Spot을 하나 등록해야 한다.** 새로 만들어진 player가 처음 들어가는 자리다.
- **C++의 entry spot만 입장 승인 callback이 필수다.** `lobby_spot_t::on_actor_join`이
  그 관문이고, 거절하도록 두면 Actor 생성 자체가 실패한다.

### 10. Location — 위치 조회

Spot과 Actor는 id로만 불렀고, 어디에 있는지는 Framework가 찾았다. 그 기록을 직접 읽는
호출이다.

```console
$ curl http://127.0.0.1:5180/locations/rooms/d50a66f2-fb52-4d4f-82a0-fdbeafd45511
{"node":"game-server-1","spotId":"d50a66f2-fb52-4d4f-82a0-fdbeafd45511"}

$ curl http://127.0.0.1:5180/locations/players/p7
{"actorId":"p7","node":"game-server-1"}

$ curl -i http://127.0.0.1:5180/locations/players/ghost
HTTP/1.1 404 Not Found
```

조회는 Location Store만 읽고 대상에게는 아무것도 보내지 않는다. 지금 메시지를 받을 수 있는
대상만 답하므로, 만들어지는 중이거나 옮겨 가는 중이면 빈 값이 온다.

### 11. STREAM과 Session-Actor 연결

외부 client가 TCP로 붙는다. framework가 아니라 connector만 링크한다.

```console
$ ./build/tutorial_stream_client
connected: true
round trip: 2ms          # STREAM request/reply
bound player: p1         # 연결을 player에 묶는다
pushed: speedy           # player가 그 연결로 밀어 준다
```

`pushed`가 핵심이다. client는 nickname 변경만 보냈고, 응답이 아니라 **player가 스스로 민
알림**을 받았다.

C++ 쪽에서 알아 둘 것은 다음과 같다.

- **session handler 등록 표면이 없다.** 모든 packet이 `on_packet` 하나로 오고, 이 tutorial은
  packet 이름으로 갈래를 나눈다. .NET·Java·Kotlin·Node는 handler를 따로 등록한다.
- **`reply_packet`은 Request에만 답한다.** 기다리는 요청이 없는 client에 밀 때는 actor 쪽에서
  `bound_session().send(...)`를 쓴다.
- **connector는 manual dispatch로 열었다.** 그래야 wait를 걸기 전에 도착한 push가 버려지지
  않고 큐에 남는다.

### 12. HTTP client

`HttpClient`는 mesh 밖에서 실행되며 `zlink::http_client` package만 링크한다. 이 CLI는
비동기 `async<T>()`, `async_raw()`, `fetch<T>()`, `download()` 종결자를 `co_await`로 기다린다.
Server와 Client를 실행한 상태에서 다음 명령으로 실행한다.

```bash title="linux"
./build/tutorial_http_client
```

```powershell title="windows"
& .\build\Release\tutorial_http_client.exe
```

출력은 다음과 같다. 방 id와 다운로드 바이트 수는 실행마다 달라진다.

```console
first request: p1 rookie
request shaping: status 200 weight 2
json body: player 200 room fbf674c7-9328-4624-a74a-7bccdab703f6 chat 202
response kinds: typed 200 raw application/json fetch anonymous
compressed response: 200 encoding-removed true
redirect: 200 p1
basic auth: without 401 with 200
download stream: chunks 1 bytes 74
upload stream: imported 3
error kinds: bad request internal_failure connection refused unavailable
```

이 출력은 framework 0.19.0 기준이다. C++ HTTP host는 gzip 및 chunked 응답을 제공하지 않으므로
6단계는 평문 응답을 확인하고 9단계는 버퍼링된 chunk 하나를 받는다.

HTTP 표면에는 admin Basic auth, 옛 player 경로 redirect, room export/import NDJSON route가
있다. admin 자격 증명은 tutorial에 설정 파일을 추가하지 않기 위해 `ops` /
`tutorial-admin`으로 하드코딩되어 있다.

```console
$ curl -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=2'
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Basic realm="tutorial-admin"

$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=2'
HTTP/1.1 200 OK
{"channel":"profile","weight":2}

$ curl -i http://127.0.0.1:5180/player/p1
HTTP/1.1 301 Moved Permanently
Location: /players/p1

$ curl http://127.0.0.1:5180/rooms/<room-id>/export
{"roomId":"<room-id>"}
{"message":"p2: hello"}

$ curl -X POST http://127.0.0.1:5180/rooms/<room-id>/import \
    -H 'Content-Type: application/x-ndjson' \
    --data-binary $'{"playerId":"p1","text":"one"}\n{"playerId":"p2","text":"two"}\n'
{"imported":2}
```

## 문서가 읽는 방식

문서는 코드를 손으로 옮겨 적지 않고 이 파일들에서 구간을 읽는다. 구간은 소스의
`--8<--` 마커가 정한다.

```
--8<-- "framework/languages/cpp/tutorial/Server/main.cpp:channel-register"
```

Spot 단계가 더한 마커는 아래와 같다.

| 마커 | 자리 |
|---|---|
| `spot-contracts` | `Shared/contracts.hpp` |
| `spot-class` · `spot-handlers` | `Server/spots/game_room.hpp` |
| `location-store` · `relocation-store` | `Server/main.cpp` |
| `object-server` · `spot-register` | `Server/main.cpp` |
| `location-store-client` · `spot-client-register` | `Client/main.cpp` |
| `spot-create-call` · `spot-message-call` | `Client/main.cpp` |
| `spot-send-call` · `spot-request-call` | `Client/main.cpp`. `spot-message-call` 안에 나뉘어 있다 |
| `instance-spot-contracts` | `Shared/contracts.hpp` |
| `instance-spot-class` · `instance-spot-handler` | `Server/spots/match_queue.hpp` |
| `instance-spot-register` | `Server/main.cpp` |
| `instance-spot-call` | `Client/main.cpp` |

마커 이름을 바꾸면 그 구간을 읽는 문서가 조용히 빈 코드 블록을 낸다. 이름을 바꿀 때는
문서를 함께 고친다. 마커 이름은 .NET tutorial과 같다.

| 마커 | 자리 |
|---|---|
| `channel-contracts` | `Shared/contracts.hpp` |
| `channel-request-handler` | `Server/channel/get_player_profile_handler.hpp` |
| `channel-send-handler` | `Server/channel/record_login_handler.hpp` |
| `mesh-register` | `Server/main.cpp` |
| `channel-register` | `Server/main.cpp` |
| `channel-client-register` | `Client/main.cpp` |
| `channel-request-call` | `Client/main.cpp` |
| `channel-send-call` | `Client/main.cpp` |
| `node-direct-contracts` | `Shared/contracts.hpp` |
| `node-direct-handler` | `Server/ops/node_status_handler.hpp` |
| `node-direct-register` | `Server/main.cpp` |
| `node-direct-call` | `Client/main.cpp` |
| `filter-implementation` | `Server/dispatch/call_log_filter.hpp` |
| `filter-register` | `Server/main.cpp` |
| `clientserver-contracts` | `Shared/contracts.hpp` |
| `clientserver-handler` | `Server/channel/issue_session_ticket_handler.hpp` |
| `clientserver-register` | `Server/main.cpp` |
| `clientserver-client-register` | `Client/main.cpp` |
| `clientserver-call` | `Client/main.cpp` |
| `fanout-contracts` | `Shared/contracts.hpp` |
| `fanout-handler` | `Server/channel/maintenance_notice_subscriber.hpp` |
| `fanout-subscribe` | `Server/main.cpp` |
| `fanout-publish-register` | `Client/main.cpp` |
| `fanout-call` | `Client/main.cpp` |
| `weight-runtime` | `Server/ops/channel_weight_handler.hpp` |
| `instance-spot-contracts` | `Shared/contracts.hpp` |
| `instance-spot-class` · `instance-spot-handler` | `Server/spots/match_queue.hpp` |
| `instance-spot-register` | `Server/main.cpp` |
| `instance-spot-call` | `Client/main.cpp` |
| `location-find` | `Client/main.cpp` |
| `actor-contracts` | `Shared/contracts.hpp` |
| `actor-class` · `actor-factory` | `Server/actors/player.hpp` |
| `entry-spot` · `actor-handlers` · `actor-push` | `Server/spots/lobby_spot.hpp` |
| `actor-send-handler` · `actor-request-handler` | `Server/spots/lobby_spot.hpp`. `actor-handlers` 안에 나뉘어 있다 |
| `actor-register` | `Server/main.cpp` |
| `actor-create-call` · `actor-send-call` · `actor-request-call` | `Client/main.cpp` |
| `stream-contracts` · `session-actor-contracts` | `Shared/contracts.hpp` |
| `session-class` · `session-handler` · `session-actor-bind` · `session-actor-relay` | `Server/sessions/game_session.hpp` |
| `stream-register` | `Server/main.cpp` |
| `stream-client` · `session-actor-client` | `StreamClient/main.cpp` |
| `http-client-create` | `HttpClient/main.cpp` |
| `http-first-request` | `HttpClient/main.cpp` |
| `http-request-shaping` | `HttpClient/main.cpp` |
| `http-json-body` | `HttpClient/main.cpp` |
| `http-response-kinds` | `HttpClient/main.cpp` |
| `http-compressed-response` | `HttpClient/main.cpp` |
| `http-redirect` | `HttpClient/main.cpp` |
| `http-basic-auth` | `HttpClient/main.cpp` |
| `http-download-stream` | `HttpClient/main.cpp` |
| `http-upload-stream` | `HttpClient/main.cpp` |
| `http-error-kinds` | `HttpClient/main.cpp` |

## .NET tutorial과 달라진 지점

같은 장면을 옮기면서 뜻이 바뀐 자리다. 이름만 다른 것(`AddRouteMesh` →
`add_route_mesh` 등)은 적지 않는다.

| 내용 | .NET | C++ |
|---|---|---|
| Location Store | Redis store를 걸고, fanout 구독자가 publisher를 자동으로 찾는다 | store가 없다. 구독자에게 publisher endpoint를 직접 적는다. 대신 mesh node에 `set_object_role(object_role_t::none)`이 필요하다. 이 값을 두지 않으면 mesh node가 Object Server가 되고, 그 role은 Location Store를 요구한다 |
| fanout handler 등록 | `AddFanoutChannel(...).AddHandler<...>()` | handler group을 거친다. channel builder에 handler를 직접 다는 API가 없다 |
| ClientServer 호출자 | `IZLinkRouteClient`가 mesh channel과 ClientServer channel을 함께 다룬다 | 타입이 나뉜다. mesh는 `route_client_t`, ClientServer는 `channel_client_t` |
| fanout publish의 topic | `Publish("broadcast", notice)` — topic을 적지 않는다 | `publish("broadcast", topic, notice)` — topic을 적는다. 구독자 handler가 듣는 topic의 기본값이 event 타입의 packet 이름이므로 그 값을 적는다 |
| mesh advertise host | `Listen("tcp://0.0.0.0:7201")`만으로 동작한다 | wildcard bind에는 `set_advertise_host`가 필요하다. fanout publisher도 마찬가지여서 `tcp://127.0.0.1:7412`로 bind한다. wildcard로 두면 startup에서 `Fanout wildcard bind host requires an advertise host`로 죽는다 |
| `NodeStatus`의 `ProcessId` | 있다 | 없다. process id를 얻는 표준 C++ API가 없어 뺐다 |
| `NodeStatus`의 `Uptime` 기준 | `Process.StartTime` | 정적 초기화 시점. handler 객체는 호출마다 새로 만들어지므로 handler의 멤버로 재면 언제나 `0s`다 |
| `ChannelName`의 빈 값 | `null`이므로 `?? "(none)"`로 바꾼다 | `std::optional`이 비어 있고, `value_or("")`로 빈 문자열을 낸다 |
| message 계약 | record 선언만으로 직렬화된다 | `to_json`/`from_json` 한 쌍이 있어야 JSON codec이 그 타입을 받는다. 그 쌍을 손으로 쓰는 덕에 C++ 멤버는 snake_case로 두고 wire의 key는 camelCase로 맞춘다 |
| 없는 node를 부른 결과 | 500 | 404 `MeshNode request target was not found` |
| runtime weight의 자리 | `Server/Program.cs`의 `app.MapPost(...)` 한 덩어리 | handler class(`Server/ops/channel_weight_handler.hpp`)와 등록(`Server/main.cpp`의 `options.http()`)으로 나뉜다. `weight-runtime` 마커는 handler 쪽에 있다 |
| runtime weight의 접근자 | property. `mesh.Channel(c).Weight = value` | getter·setter 한 쌍. `mesh.channel(c).weight(value)`로 쓰고 `weight()`로 읽는다 |
| runtime weight의 `value` | `int value` 인자로 선언하면 query string이 묶인다 | model binding이 없다. `request.query_values`에서 손으로 꺼내고, 없는 경우도 handler가 답해야 한다 |
| Server의 HTTP | 원래 `WebApplication`이라 HTTP가 이미 있다 | Server가 HTTP를 열지 않았다. 이 endpoint 때문에 `options.http().listen("http://127.0.0.1:5181")`을 처음 추가했다 |
| 아직 옮기지 않은 장 | User Spot, Instance Spot, 모니터링 | 없다 |
