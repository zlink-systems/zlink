[English](README.md) | **한국어**

# ZLink Engine Lobby server

Unity를 비롯한 게임 엔진 client가 공용 lobby에 참가해 chat을 주고받는 .NET server다. 하나의
STREAM listener가 `Ping`/`Pong`, `Join`/`Joined`와 `Chat`/`ChatNotify` 흐름을 제공하며,
session에 bind된 Actor가 lobby fan-out을 수행한다.

요구 사항은 .NET 8 SDK, Docker, curl과 Python 3이다. 이 디렉터리는 공개 NuGet package만
참조하므로 monorepo 밖으로 export한 뒤에도 같은 명령으로 실행된다.

## 내려받기와 설치

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh install
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action install
```

## 빌드

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh build
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action build
```

## 실행

Runner는 이 실행만 사용하는 Redis container를 만들고 RouteMesh, WebSocket STREAM과 HTTP
readiness port를 배정한 뒤 server를 시작한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh run
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action run
```

## 검증

먼저 curl로 readiness를 확인한다. 이어서 같은 project의 C# probe가 connector 두 개로
`Ping`, 두 번의 `Join`, `Chat`과 두 client의 `ChatNotify` payload를 직접 검증한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh verify
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action verify
```

성공하면 마지막 줄에 `engine-lobby-probe=ok`가 출력된다.

## 종료

실행 절에서 기록한 정확한 server PID와 Redis container ID만 종료한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh stop
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action stop
```

실패 진단은 `.run/server.log`과 `.run/server.err.log`에 남는다.
