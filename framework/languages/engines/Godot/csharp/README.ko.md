[English](README.md) | **한국어**

# ZLink Engine Lobby Godot sample

Godot 4 C# scene이 공용 Engine Lobby server에 연결한다. `EngineLobbyNode`는 frame마다
connector callback을 Godot main thread에서 처리하고, `PingReq` → `JoinReq` → `ChatMsg` 흐름의
`ChatNotify`를 화면에 표시한다.

Packet 이름과 JSON field는 [Engine Lobby 공통 계약](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)을 따른다.

## Version과 package

- Godot 4.4.1 .NET edition
- .NET 8 SDK와 `Godot.NET.Sdk` 4.4.1
- `Zlink.Stream.Connector`: [`EngineLobby.csproj`](EngineLobby.csproj)의 `PackageReference`가 version을 정한다.

`EngineLobby.csproj`가 두 NuGet package를 복원한다. Godot Web export는 이 .NET project의
검증 대상이 아니다.

## 설치

```bash
dotnet restore EngineLobby.csproj
```

## 빌드

```bash
dotnet build EngineLobby.csproj -c Release
dotnet build Validation/EngineLobbyValidation.csproj -c Release
```

## Scene 실행과 검증

1. 별도 [zlink-engine-server 저장소](https://github.com/zlink-systems/zlink-engine-server/blob/main/README.ko.md)의 README에 따라 server를 설치·빌드·실행하고 `.run/stream.port`를 읽는다.
2. `EngineLobby.tscn`의 root node `Endpoint`를 `ws://127.0.0.1:<stream.port>`로 설정한다. 기본값은 `ws://127.0.0.1:22700`이다.
3. Godot 4.4.1 .NET editor에서 project를 빌드한 뒤 scene을 실행한다. 다음 Linux 명령은 같은 editor build와 scene 실행을 display 없이 수행한다. `GODOT`에는 Godot 4.4.1 .NET 실행 파일의 경로를 지정한다.

```bash
"$GODOT" --headless --path . --build-solutions --quit-after 120
"$GODOT" --headless --path . --quit-after 600
```

Windows에서는 Godot 4.4.1 .NET의 `_console.exe` 경로를 `$godot`에 지정하고 PowerShell에서 실행한다. 출력은 `godot-csharp.log`와 `godot-csharp.err.log`에 기록한다.

```powershell
$godot = 'C:\path\to\Godot_v4.4.1-stable_mono_win64_console.exe'
Start-Process $godot -ArgumentList @('--headless','--path',(Get-Location).Path,'--build-solutions','--quit-after','120') -Wait -WindowStyle Hidden -RedirectStandardOutput godot-build.log -RedirectStandardError godot-build.err.log
Start-Process $godot -ArgumentList @('--headless','--path',(Get-Location).Path,'--quit-after','600') -Wait -WindowStyle Hidden -RedirectStandardOutput godot-csharp.log -RedirectStandardError godot-csharp.err.log
```

Godot 출력에서 `JoinRes name=godot-player`(Ping 응답을 확인한 뒤 Join한다), `ChatNotify name=godot-player text=hello from Godot`를 확인한다. Label도 `joined as godot-player (...)`를 거쳐 `godot-player: hello from Godot`로 바뀐다. scene 실행 중 server 로그에 새 `client connected:` 줄이 추가되는지 확인한다. 검증을 마치면 server README의 종료 절차를 실행한다.

## Headless connector 검증

`Validation`은 scene이 사용하는 `EngineLobbyClient.cs`를 그대로 compile한다. 준비된 server의
WebSocket endpoint를 인자로 주면 client 두 개로 Ping, Join과 양쪽 ChatNotify payload를 확인한다.

```bash
dotnet run --project Validation/EngineLobbyValidation.csproj -c Release -- \
  ws://127.0.0.1:<stream.port>
```

성공 출력은 `godot-engine-lobby-validation=ok`다. 이 검증은 Godot scene tree, editor의 C#
assembly load와 실제 화면 표시를 실행하지 않는다.
