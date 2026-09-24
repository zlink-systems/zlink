[English](README.md) | **한국어**

# ZLink Engine Lobby Godot C++ 샘플

Godot 4.4.1 GDExtension scene은 공개 C++ Godot stream connector adapter를 사용한다.
`EngineLobbyNode`는 `_process`에서 `dispatch()`를 호출해 Godot main thread에서 callback을 처리한다.
메시지는 `PingReq` → `PingRes` → `JoinReq` → `JoinRes` → `ChatMsg` → `ChatNotify` 순서로 교환한다.
Packet과 JSON field는 [Engine Lobby 공통 계약](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)을 따른다.

## 의존성과 설치

- Godot 4.4.1, 같은 버전의 `godot-cpp`, C++20 compiler, CMake 3.24 이상
- Linux: GCC 13 이상과 Ninja 또는 Make
- Windows: Visual Studio 2022 C++ workload와 PowerShell 7

이 저장소는 Godot project만 포함한다. [C++ framework v0.25.0 release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.25.0)의
플랫폼별 prebuilt에는 Core, C++ binding, stream connector와 CMake package가 포함된다.
같은 release의 source archive에 있는 기존 Godot adapter만 prebuilt에 연결해 빌드한다.
Monorepo checkout이나 별도 package manager는 필요하지 않다.

다음 명령은 이 저장소의 `cpp/`에서 실행한다.

**Linux · WSL — bash**

```bash
mkdir -p .zlink
touch .zlink/.gdignore
base=https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v0.25.0
for asset in zlink-framework-cpp-0.25.0-linux-x64.tar.gz zlink-framework-cpp-0.25.0.tar.gz; do
  curl -fL "$base/$asset" -o ".zlink/$asset"
  curl -fL "$base/$asset.sha256" -o ".zlink/$asset.sha256"
  (cd .zlink && sha256sum -c "$asset.sha256")
  tar -xzf ".zlink/$asset" -C .zlink
done
git clone --branch godot-4.4.1-stable --depth 1 https://github.com/godotengine/godot-cpp.git .zlink/godot-cpp
```

**Windows — PowerShell 7**

```powershell
New-Item -ItemType Directory -Force .zlink | Out-Null
New-Item -ItemType File -Force .zlink/.gdignore | Out-Null
$base = 'https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v0.25.0'
foreach ($asset in @('zlink-framework-cpp-0.25.0-windows-x64.tar.gz', 'zlink-framework-cpp-0.25.0.tar.gz')) {
  curl.exe -fL "$base/$asset" -o ".zlink/$asset"
  curl.exe -fL "$base/$asset.sha256" -o ".zlink/$asset.sha256"
  $expected = ((Get-Content ".zlink/$asset.sha256" -Raw).Trim() -split '\s+')[0]
  if ((Get-FileHash ".zlink/$asset" -Algorithm SHA256).Hash -ine $expected) { throw "SHA256 mismatch: $asset" }
  tar -xzf ".zlink/$asset" -C .zlink
}
git clone --branch godot-4.4.1-stable --depth 1 https://github.com/godotengine/godot-cpp.git .zlink/godot-cpp
```

## 빌드

**Linux · WSL — bash**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/.zlink/zlink-framework-cpp-0.25.0-linux-x64" \
  -DGODOT_CPP_ROOT="$PWD/.zlink/godot-cpp" \
  -DZLINK_FRAMEWORK_CPP_SOURCE_ROOT="$PWD/.zlink/zlink-framework-cpp-0.25.0"
cmake --build build --target engine_lobby_godot -j8
```

**Windows — PowerShell 7**

```powershell
cmake -S . -B build `
  "-DCMAKE_PREFIX_PATH=$PWD/.zlink/zlink-framework-cpp-0.25.0-windows-x64" `
  "-DGODOT_CPP_ROOT=$PWD/.zlink/godot-cpp" `
  "-DZLINK_FRAMEWORK_CPP_SOURCE_ROOT=$PWD/.zlink/zlink-framework-cpp-0.25.0"
cmake --build build --config Release --target engine_lobby_godot --parallel 8
```

GDExtension library와 Windows runtime DLL은 `bin/`에 생성된다.
`engine_lobby.gdextension`이 플랫폼별 library를 로드한다.

## 실행과 확인

1. 별도의 [`zlink-engine-server` 저장소](https://github.com/zlink-systems/zlink-engine-server)를 clone하고
   해당 저장소의 [README.ko.md](https://github.com/zlink-systems/zlink-engine-server/blob/main/README.ko.md)에
   따라 server를 시작한다. `.run/stream.port` 값을 확인한다.
2. Port가 기본값 22700과 다르면 `EngineLobby.tscn`의 `EngineLobbyNode` `endpoint`를
   `ws://127.0.0.1:<stream.port>`로 설정한다.
3. Godot 4.4.1로 project를 한 번 import하고 scene을 실행한다. `Status` Label과 Godot 출력의
   다음 항목을 확인한다. Linux에서는 `GODOT`에 내려받은 Godot 4.4.1 실행 파일의 절대 경로를 지정한다.

**Linux · WSL — bash**

```bash
GODOT=/path/to/Godot_v4.4.1-stable_mono_linux.x86_64
"$GODOT" --headless --editor --path . --quit-after 120
"$GODOT" --headless --path . --quit-after 600
```

**Windows — PowerShell 7**

```powershell
$godot = 'C:\path\to\Godot_v4.4.1-stable_mono_win64_console.exe'
$import = Start-Process -FilePath $godot -ArgumentList @('--headless', '--editor', '--path', '.', '--quit-after', '120') -WorkingDirectory $PWD -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput 'godot-import.log' -RedirectStandardError 'godot-import.err.log'
if ($import.ExitCode -ne 0) { throw "Godot import failed: $($import.ExitCode)" }
$scene = Start-Process -FilePath $godot -ArgumentList @('--headless', '--path', '.', '--quit-after', '600') -WorkingDirectory $PWD -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput 'godot-scene.log' -RedirectStandardError 'godot-scene.err.log'
if ($scene.ExitCode -ne 0) { throw "Godot scene failed: $($scene.ExitCode)" }
Get-Content godot-scene.log
```

```text
PingRes sentAtUnixMs=1000
JoinRes name=godot-player actorId=<server-assigned-id>
ChatNotify godot-player: hello from Godot C++
```

Godot 출력에 `Engine Lobby failed:`가 있으면 해당 오류를 확인한다. Server 로그에 scene 실행
중 새로운 `client connected: <sessionId>` 행이 있어야 한다.
Server는 해당 저장소 README의 stop 절차로 종료한다.
