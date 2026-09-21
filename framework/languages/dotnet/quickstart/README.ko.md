[English](./README.md) | [한국어](./README.ko.md)

# ZLink .NET quickstart

가장 작은 project다. location store 없이 client가 server endpoint를 직접 지정하고, 두
process가 channel로 한 번 호출한다. 사이트 페이지 `framework/doc/framework/dotnet/quickstart.ko.md`가
이 파일들에서 코드 블록을 읽어 간다. 이 디렉터리는 `zlink-dotnet-examples` 저장소의
`quickstart/`다.

| | 목적 |
|---|---|
| **quickstart** (여기) | 패키지를 설치하고 기능을 더하지 않은 상태로 첫 응답까지 확인한다 |
| tutorial (`tutorial/`) | 기능을 차례로 추가한다. 기능별 guide가 이 코드를 읽는다 |
| samples (`samples/`) | 완결된 업무 흐름을 보이는 application을 제공한다 |

## 전제 조건

- .NET SDK 8.0 이상. project의 target은 `net8.0`이다.
- `nuget.org`에 접속할 수 있어야 한다. `nuget.config`가 이 feed를 사용한다.
- Redis나 다른 외부 service는 필요하지 않다.

## 내려받기와 설치

[`zlink-dotnet-examples`](https://github.com/zlink-systems/zlink-dotnet-examples) 저장소를
clone한다. 아래 명령은 저장소의 `quickstart/`에서 실행한다.

`Directory.Packages.props`가 `Zlink.Framework`와 `Zlink.Framework.AspNetCore`의 버전을
고정한다. `Zlink` binding은 이 파일에 적지 않고 `Zlink.Framework`의 전이 의존에 맡긴다.

## 빌드

```bash title="linux"
dotnet build
```

```powershell title="windows"
dotnet build
```

## 실행

서버를 먼저 실행하고 별도 terminal에서 client를 실행한다. server는 `tcp://0.0.0.0:7101`에서
듣고 `greeting` channel을 처리한다. client는 `tcp://0.0.0.0:7102`에서 듣고
`tcp://127.0.0.1:7101`에 연결하며, `http://127.0.0.1:5080`에서 `GET /hello/{name}`을
제공한다.

```bash title="linux"
dotnet run --project Server/Server.csproj > server.log 2>&1 &
dotnet run --project Client/Client.csproj > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5080/hello/world > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:5080/hello/world
```

```powershell title="windows"
Start-Process -NoNewWindow dotnet -ArgumentList 'run','--project','Server/Server.csproj' -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Start-Process -NoNewWindow dotnet -ArgumentList 'run','--project','Client/Client.csproj' -RedirectStandardOutput client.log -RedirectStandardError client.err.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5080/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
$answer
```

## 검증

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

endpoint는 HTTP 상태 코드 200과 `"hello, world"`를 반환한다.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| 7101, 7102, 5080이 이미 사용 중이다 | 이전 Server 또는 Client process를 종료한 뒤 pair를 다시 실행한다 |
| curl 요청이 연결되지 않는다 | Server를 먼저 실행한 뒤 Client를 실행하고 process 출력을 확인한다 |
| 요청에 대상이 없다 | client의 `PeerConnections.Connect` endpoint와 server의 `Listen` endpoint를 같게 둔다 |
| handler가 호출되지 않는다 | `greeting` channel에 `HelloHandler`를 명시적으로 등록한다 |

## 구성

| 경로 | 내용 |
|---|---|
| `Shared/` | 두 process가 공유하는 `Hello`와 `Greeting` record 계약 |
| `Server/` | `greeting` handler를 등록하고 7101 port에서 듣는 process |
| `Client/` | server에 연결하고 5080 port에서 `GET /hello/{name}`을 제공하는 process |
| `Directory.Packages.props` | framework package의 중앙 version 고정 |
| `nuget.config` | NuGet package source 설정 |

## 내 프로젝트에 옮길 것

- 명시적인 `Zlink.Framework`와 `Zlink.Framework.AspNetCore` version을 포함한
  `Directory.Packages.props`. package 계약이 직접 참조를 요구하지 않으면 `Zlink` binding은
  framework의 전이 의존에 맡긴다.
- record 기반 message 계약을 정의하는 `Shared/Contracts.cs`.
- mesh 이름, `Listen`, `Channel("greeting").Server()`, 명시적인 `AddRequestHandler` 등록을
  담은 `Server/Program.cs`의 `AddZLinkFramework` 블록.
- `Channel("greeting").Client()`, `PeerConnections.Connect`,
  `RequestToChannel(...).Async<Greeting>()` 호출을 담은 `Client/Program.cs`의
  `AddZLinkFramework` 블록.
- 실제 service에서는 수동 peer connection을 Redis와 같은 location store로 바꾸는 것이
  일반적이다. 이 quickstart는 해당 service 의존성을 넣지 않는다.
