# ZLink Node.js Framework Samples

Node.js/NestJS framework의 공개 API를 사용하는 sample이다. `zlink-node-examples` 저장소를
clone하고 `samples/`에서 빌드하고 실행한다.

## 샘플 목록

| 샘플 | 보여 주는 기능 | client 실행 환경 | 연결 구성 |
|---|---|---|---|
| `Bingo.Ts` | Session, Entry Spot, room Spot, player Actor, timer와 bound-session push | Chromium | Redis location store |
| `TicTacToe.Ts` | API 2개와 Play 2개의 scale-out, room 조회와 실시간 게임 | Chromium | 수동 MeshNode peer와 Redis room route store |
| `SupportChat.Ts` | conversation owner, 상담 배정, reconnect, idle timer와 종료 알림 | Chromium | Redis location store |
| `DeliveryDispatch.Ts` | 배송 배차, timeout 재배정과 고객·기사 상태 알림 | Chromium | Redis location store |
| `GameQuest.Ts` | player별 quest owner Spot, event stream과 조회 모델 | Chromium | Redis location store |
| `ShoppingMall.Ts` | 주문 workflow, event stream, HTTP 조회 모델과 fanout event | Node.js HTTP client | Redis location store |
| `ZoneWorld` | Actor의 zone 이동, Logical Multicast, Node direct, runtime event와 관제 UI | headless Node.js scenario와 자체 Chromium client | Redis location store |

TicTacToe만 MeshNode peer endpoint를 수동으로 설정한다. 다른 sample은 Spot과 Actor 위치를 찾고 peer를
구성할 때 Redis Location Store를 사용한다.

업무 흐름, 메시지 계약과 smoke 순서 같은 언어 중립 내용은 저장소의
[framework 공통 sample 문서](../../../doc/framework/common/sample/README.ko.md)가 소유한다(저장소
안에서만 열린다 — 실행에는 필요 없다).

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

- **Node.js 22 이상.** `@zlink-systems/zlink`가 `"engines": { "node": ">=22" }`를 선언한다.
  `node --version`으로 확인한다. `@zlink-systems/framework` 0.18.1부터는 Windows용
  prebuild(#656)가 있는 `zlink@1.2.1`을 고정한다 — 그 전 framework release는 아직
  `1.2.0`만 받는다(아래 「문제 해결」).
- **Docker Desktop(또는 Docker Engine)이 실행 중이어야 한다.** 추가 요구 사항은 없다. 각 sample runner가
  Redis container(`redis:7.2-alpine`)를 직접 만들고 종료 시 제거하므로, Redis를 따로 설치하거나
  실행할 필요가 없다.
- **독립 sample에서 Chromium이 필요하면 Playwright를 그 sample 디렉터리에 설치한다.** `ShoppingMall.Ts`를 뺀
  나머지 sample(`ZoneWorld` 포함)이 client를 실제 Chromium에서 실행한다 — 위 표의 「client 실행
  환경」 참고. `npm install` 뒤 그 sample 디렉터리에서 `npm run browser:install`을 한 번 실행하면
  된다. 저장소 안에서는 아래 workspace 명령을 사용한다. `ShoppingMall.Ts`는 Node.js HTTP
  client이므로 이 설치가 필요 없다.

## 내려받기와 설치

`zlink-node-examples` 저장소를 clone하면 `samples/` 안에
`Bingo.Ts/`, `TicTacToe.Ts/`, … sample 디렉터리가 있다. 각 sample은
`@zlink-systems/*`를 그 샘플이 고정한 버전으로 npm registry에서 받는다. workspace가 아니므로
샘플마다 따로 설치한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
cd Bingo.Ts
npm install
npm run browser:install   # Chromium을 쓰는 샘플만(ShoppingMall.Ts 제외)
```

**Windows — PowerShell 7**

```powershell title="windows"
Set-Location Bingo.Ts
npm install
npm run browser:install   # Chromium을 쓰는 샘플만(ShoppingMall.Ts 제외)
```

경로의 `Bingo.Ts` 자리에 다른 sample 이름을 차례로 넣어 반복한다(`DeliveryDispatch.Ts`,
`GameQuest.Ts`, `ShoppingMall.Ts`, `SupportChat.Ts`, `TicTacToe.Ts`, `ZoneWorld`). 확인할
sample만 설치해도 된다.

저장소 안에서 Node framework workspace를 함께 쓰는 경우에는 workspace root에서 한 번에
준비할 수 있다. workspace가 `.artifacts/node-install/npm/`의 로컬 tarball을 참조하므로
`npm ci` 전에 binding과 HTTP client package를 만든다. sample이 연결할 framework `dist`도
빌드한다. sample 디렉터리마다 `npm install`을 실행하지 않는다.

**Linux · WSL — bash (저장소 root에서)**

```bash title="linux"
scripts/local-package/build-wsl.sh node
scripts/local-package/http-client/build-wsl.sh node
cd framework/languages/node
npm ci
npm run build
npm run browser:install
```

**Windows — PowerShell 7 (저장소 root에서)**

```powershell title="windows"
$prefix = & .\scripts\local-package\core\fetch-release.ps1
.\scripts\local-package\node\build-windows.ps1 -CorePrefix $prefix
.\framework\languages\node\build-windows.ps1 -SkipSamples
Set-Location framework/languages/node
npm ci
npm run browser:install
```

Windows framework 빌드가 로컬 HTTP client tarball과 framework `dist`를 만든다.

## 빌드

설치 뒤에 sample을 따로 빌드하지 않는다. 각 샘플의 `npm run sample`(→ `run_sample.*`가 부른다)이 실행 전에
`npm run build`를 자동으로 호출한다. Windows에서 여러 샘플을 미리 한꺼번에 빌드해 두려면
`build_samples.ps1`을 쓴다.

**Windows — PowerShell 7**

```powershell title="windows"
./build_samples.ps1 Bingo.Ts TicTacToe.Ts
# 인자를 생략하면 일곱 샘플을 모두 빌드한다.
```

Linux·WSL에는 별도 빌드 단계가 없다 — 아래 「실행」의 `run_sample.sh`가 실행 전에 자동으로
빌드한다.

## 실행

샘플마다 `run_sample.sh`(Linux·WSL)와 `run_sample.ps1`(Windows)이 있고, 한 번의 호출은 샘플
하나를 실행한다. 이 디렉터리에서 그대로 호출한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./Bingo.Ts/run_sample.sh
```

**Windows — PowerShell 7**

```powershell title="windows"
./Bingo.Ts/run_sample.ps1
```

모든 sample을 확인하려면 경로의 `Bingo.Ts` 자리에 `DeliveryDispatch.Ts`, `GameQuest.Ts`,
`ShoppingMall.Ts`, `SupportChat.Ts`, `TicTacToe.Ts`, `ZoneWorld`를 차례로 넣어 각각 실행한다
(`ZoneWorld`에만 `.Ts` 접미사가 없다). 실행 방법의 세부 기준은 저장소의
[공통 sample 문서](../../../doc/framework/common/sample/README.ko.md) 「샘플 실행 스크립트와
Redis 격리 기준」 절이 소유한다.

Runner는 역할별 설정 파일 생성, server 시작, Redis container 실행, readiness 확인, client
self-check와 정리를 모두 담당한다 — Redis를 미리 실행할 필요가 없다. 예를 들어 Bingo의
전체 client 흐름은 [`Bingo.Ts/Client/bingo-client-scenario.ts`](Bingo.Ts/Client/bingo-client-scenario.ts)에서
확인할 수 있다.

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

각 runner는 성공하면 표준 출력 마지막 줄에 `PASS <Sample>`을 기록하고 종료 코드 `0`으로 끝난다.

```
PASS TicTacToe.Ts
```

실패하면 `PASS`를 기록하지 않고, 실패한 역할과 검증 항목을 표준 오류에 기록한 뒤 0이 아닌 종료
코드로 끝난다. 어떤 경우에도 자신이 시작한 server, Chromium과 Redis container는 정리한다.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| `docker: Cannot connect to the Docker daemon` | Docker Desktop(또는 dockerd)이 꺼져 있다. 띄운 뒤 다시 실행한다 |
| `Package mode requires @zlink-systems/... Run npm install in ...` | 그 샘플 디렉터리에서 `npm install`을 하지 않았다. 실행 전 위 「내려받기와 설치」를 먼저 한다 |
| `npm error gyp ERR! ... ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core ... package prefix`(Windows) | 이전 framework release가 고정한 `@zlink-systems/framework`가 아직 0.18.1 미만이라 `zlink@1.2.0`만 받는다 — win32-x64 prebuild(#656)는 framework 0.18.1(`zlink@1.2.1`)부터다. 그 버전으로 다시 받거나 WSL에서 실행한다 |
| 위와 같은 오류(macOS) | `@zlink-systems/zlink@1.2.1`에도 아직 `darwin-*` prebuild가 없다. Linux(x64)나 Windows(0.18.1부터)에서 실행한다 |
| Chromium을 쓰는 샘플에서 `browserType.launch: Executable doesn't exist` | 그 샘플 디렉터리에서 `npm run browser:install`을 하지 않았다 |
| 포트 충돌(`EADDRINUSE`) | runner가 매번 임의 loopback 포트를 선택하므로 같은 sample을 동시에 실행하지 않으면 드물다. 다시 실행하면 대개 해결된다 |
| Linux에서 Playwright가 `Host system is missing dependencies` | Playwright가 요구하는 시스템 라이브러리가 없다. `npx playwright install-deps chromium`으로 설치한다(관리자 권한 필요) |
| `EBADENGINE`(Node 버전 경고) | Node.js 22 미만이다. 위 「전제 조건」대로 22 이상으로 올린다 |

## 문서 위치

업무 흐름, 메시지 계약과 smoke 순서는 저장소의 공통 sample 문서가 소유한다. Node 샘플에 공통
내용과 다른 설정이나 실행 절차가 없으면 개별 sample 디렉터리에 README를 반복해서 두지 않는다.

## MeshNode와 channel

물리 mesh는 process마다 MeshNode 하나로 구성한다. ChannelName은 그 MeshNode가 참여하는
논리 service group이며 별도 ROUTER endpoint를 만들지 않는다. Node direct, ChannelName select-one,
Spot, Actor와 Logical Multicast는 같은 MeshNode를 사용한다. 모든 구독자에게 전달하는 classic
fanout은 별도 PUB/SUB channel이다.

## 브라우저 client 경계

Stream Connector를 사용하는 client는 브라우저용 ESM bundle로 만들어 실제 Chromium에서 실행한다.
Node.js는 bundle 생성, 정적 파일 제공과 headless Chromium 실행을 담당하며 Stream Connector의 client
runtime으로 사용되지 않는다.

`ZoneWorld/Browser/`가 이 언어의 browser UI다 — 저장소 안에서는 모든 언어 server에 연결하는
공유 TypeScript 소스(`framework/languages/shared_sample/zoneworld/client/`)와 같은 내용이고,
이 디렉터리 하나만으로도 완결된다. 자세한 connector 사용법은 저장소의
[TypeScript Stream Connector guide](../../../doc/framework/node/guide/stream-connector/README.ko.md)를
참고한다.
