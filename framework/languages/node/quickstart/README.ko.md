[English](./README.md) | [한국어](./README.ko.md)

# ZLink Node/TypeScript quickstart

가장 작은 project다. location store 없이 client가 server endpoint를 직접 지정하고, 두
process가 channel로 한 번 호출한다. 사이트 페이지 `framework/doc/framework/node/quickstart.ko.md`가
이 파일들에서 코드 블록을 읽어 간다. 이 디렉터리는 `zlink-node-examples` 저장소의
`quickstart/`다.

| | 목적 |
|---|---|
| **quickstart** (여기) | 패키지를 설치하고 기능을 더하지 않은 상태로 첫 응답까지 확인한다 |
| tutorial (`tutorial/`) | 기능을 차례로 추가한다. 기능별 guide가 이 코드를 읽는다 |
| samples (`samples/`) | 완결된 업무 흐름을 보이는 application을 제공한다 |

## 전제 조건

- Node.js 22 이상. framework가 사용하는 게시 `@zlink-systems/zlink` binding이 이 runtime
  version을 요구한다.
- `registry.npmjs.org`에 접속할 수 있어야 한다.
- Redis나 다른 외부 service는 필요하지 않다.

## 내려받기와 설치

[`zlink-node-examples`](https://github.com/zlink-systems/zlink-node-examples) 저장소를
clone한다. 아래 명령은 저장소의 `quickstart/`에서 실행한다.

`package.json`이 `@zlink-systems/framework`, `@zlink-systems/nestjs`, `@nestjs/common`,
`@nestjs/core`, `reflect-metadata`와 `devDependencies`의 TypeScript·`@types/node` 버전을 고정한다.
`@zlink-systems/zlink` binding은 목록에 넣지 않고 `@zlink-systems/framework`의 전이 의존에
맡긴다.

## 빌드

```bash title="linux"
npm install
npm run build
```

```powershell title="windows"
npm install
npm run build
```

## 실행

서버를 먼저 실행하고 별도 terminal에서 client를 실행한다. server는 `tcp://0.0.0.0:7101`에서
듣고 `greeting` channel을 처리한다. client는 `tcp://0.0.0.0:7102`에서 듣고
`tcp://127.0.0.1:7101`에 연결하며, `http://127.0.0.1:5080`에서 `GET /hello/{name}`을
제공한다.

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
| 요청에 대상이 없다 | `peerConnections().connect` endpoint와 server의 `listen` endpoint를 같게 둔다 |
| handler가 실행되기 전에 요청이 실패한다 | object literal 대신 `new Hello(name)`을 보낸다 |
| handler가 호출되지 않는다 | `addRequestHandler`로 `HelloHandler`를 등록하고 Nest module의 `providers`에도 넣는다 |
| wildcard endpoint를 시작할 수 없다 | wildcard `.listen(...)` 뒤에 `.setAdvertiseHost('127.0.0.1')`를 둔다 |

## 구성

| 경로 | 내용 |
|---|---|
| `Shared/contracts.ts` | 두 process가 공유하는 `Hello` class와 `Greeting` 계약 |
| `Server/main.ts` | `greeting` handler를 등록하고 7101 port에서 듣는 process |
| `Client/main.ts` | server에 연결하고 5080 port에서 `GET /hello/{name}`을 제공하는 process |
| `package.json` | runtime·development package pin과 세 npm script |

## 내 프로젝트에 옮길 것

- `package.json`의 exact pin. framework package 계약이 직접 의존성을 요구하지 않으면
  `@zlink-systems/zlink`는 전이 의존으로 둔다.
- packet-name resolution이 구체적인 constructor를 사용하도록 request payload를 class로
  정의하는 `Shared/contracts.ts`. reply 계약은 decode되는 shape와 맞춘다.
- mesh 이름, `.listen(...)`, `.setAdvertiseHost(...)`,
  `channel(...).server().addRequestHandler(...)` 순서와 handler class의 `providers` 등록을
  유지하는 `Server/main.ts`의 `ZLinkModule.forRootFactory` 블록.
- `channel(...).client()`, `peerConnections().connect(...)`, `ZLINK_ROUTE_CLIENT` 조회,
  `requestToChannel(...).submit<Greeting>()` 호출을 유지하는 `Client/main.ts`.
- 실제 service에서는 수동 peer connection을 Redis와 같은 location store로 바꾸는 것이
  일반적이다. 이 quickstart는 해당 service 의존성을 넣지 않는다.
