[English](./README.md) | [한국어](./README.ko.md)

# C++ Quickstart

가장 단순한 프로젝트다. 두 process가 Location Store 없이 channel로 한 번 호출한다 — client가
server endpoint를 직접 지정한다. 사이트의 `framework/doc/framework/cpp/quickstart.ko.md` 페이지는
이 파일에서 코드 블록을 읽는다.

이 디렉터리는 `zlink-cpp-examples` 저장소의 `quickstart/`다. 설치는 `tutorial/`·`samples/`와
같다: `bootstrap.cmake` 하나, GitHub Release의 Core prebuilt, binding·framework 소스 아카이브,
그리고 Conan.

| | 목적 |
|---|---|
| **quickstart** (여기) | 설치와 첫 응답 확인. 기능을 추가하지 않는다 |
| tutorial (`tutorial/`) | 기능을 단계별로 추가한다. 기능별 guide가 이 코드를 읽는다 |
| samples (`samples/`) | 완결된 업무 흐름을 보이는 application |

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

tutorial과 같지만 Docker는 필요 없다(이 프로젝트는 Redis를 쓰지 않는다).

| 도구 | Windows | Linux / WSL |
|---|---|---|
| C++20 컴파일러 | Visual Studio 2022 17.4 이상 또는 Visual Studio 2026, **C++를 사용한 데스크톱 개발** 워크로드. 2026-09 현재 2026의 msvc 195에는 ConanCenter 바이너리가 없어 첫 bootstrap이 서드파티 library를 source에서 빌드하므로 약 20분 걸린다. 2022는 바이너리를 내려받아 약 7분 걸린다 | GCC 13 이상 |
| CMake | 3.24 이상 | 3.24 이상 |
| Ninja | 필요 없음 | 권장. 없으면 Makefile을 쓴다 |
| Conan 2 | `pipx install conan` 뒤 `pipx ensurepath`를 실행하고 새 terminal을 연다. `py -m pip install --user conan`을 썼으면 Python `Scripts` directory를 PATH에 넣는다. `conan --version`으로 확인한다 | `pipx install conan` 뒤 `pipx ensurepath`를 실행하고 새 terminal을 연다. `python3 -m pip install --user conan`을 썼으면 Python user `bin` directory를 PATH에 넣는다. `conan --version`으로 확인한다 |

Conan은 ConanCenter의 서드파티 바이너리를 받는다. Visual Studio 2022에서는 **첫 bootstrap에 약 7분**,
2026의 msvc 195처럼 바이너리가 없는 toolset에서는 서드파티를 source에서 빌드하므로 **약 20분** 걸린다.
`tutorial/`이나 `samples/`에서 bootstrap을 이미 실행했으면 다시 빌드하지 말고 해당 tree를 재사용한다:
`cmake -DZLINK_ROOT=../tutorial/.zlink -P bootstrap.cmake`.

## 내려받기와 설치

[`zlink-cpp-examples`](https://github.com/zlink-systems/zlink-cpp-examples) 저장소를 clone한다.
아래 명령은 모두 그 저장소의 `quickstart/` 안에서 실행한다.

설치는 [빌드](#빌드) 블록 첫 줄의 `bootstrap.cmake`로 처리한다. 이 script는 GitHub
Release에서 이 플랫폼의 Core prebuilt, C++ binding 소스, framework 소스를 받아 binding과
framework를 빌드해 `.zlink/install/`에 설치하고, 이 project를 `build/`에 구성한다. framework
버전만 script에 적혀 있고 Core·binding·서드파티 버전은 framework 아카이브가 정한다. 기본은
Conan이고 vcpkg fallback은 `-DZLINK_PACKAGE_MANAGER=vcpkg`로 고른다. 두 번째 실행부터는 받은
파일과 빌드 결과를 그대로 사용한다. 처음부터 다시 설치하려면 `.zlink/`와 `build/`를 지운다.

## 빌드

**Linux · macOS · WSL — bash**

```bash title="linux"
cmake -P bootstrap.cmake
cmake --build build --parallel
```

**Windows — PowerShell 7**

```powershell title="windows"
cmake -P bootstrap.cmake
cmake --build build --config Release --parallel
```

실행 파일은 Windows의 `build\Release\`, Linux의 `build/`에 생성된다.

## 실행

server는 `tcp://0.0.0.0:7301`에서 대기한다. client는 `7302`에서 대기하고 server에 연결한 뒤
`http://127.0.0.1:5083`에서 `GET /hello/{name}`을 제공한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./build/quickstart_server > server.log 2>&1 &
echo $! > server.pid
./build/quickstart_client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5083/hello/world > /dev/null && break; sleep 1; done
```

**Windows — PowerShell 7**

```powershell title="windows"
$server = Start-Process -NoNewWindow .\build\Release\quickstart_server.exe -RedirectStandardOutput server.out -RedirectStandardError server.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -NoNewWindow .\build\Release\quickstart_client.exe -RedirectStandardOutput client.out -RedirectStandardError client.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5083/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
```

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5083/hello/world | grep -q '"hello, world"'
echo "quickstart=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
if ((curl.exe -s http://127.0.0.1:5083/hello/world) -notmatch '"hello, world"') { throw 'quickstart failed' }
Write-Output 'quickstart=ok'
```

응답은 `"hello, world"`, 상태 코드 200이다.

## 종료

실행 절에서 시작한 process를 종료한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
```

**Windows — PowerShell 7**

```powershell title="windows"
Get-Content client.pid, server.pid | ForEach-Object {
  if ($_ -match '^\d+$') { taskkill /PID $_ /T /F 2>$null | Out-Null }
}
Get-Job | Stop-Job -ErrorAction SilentlyContinue
```

## 문제 해결

설치 단계의 증상(Conan 없음, 지원하지 않는 컴파일러 설정, download 실패, 컴파일러 없음,
`RuntimeLibrary` 불일치, `STATUS_DLL_NOT_FOUND`)은 tutorial과 같으므로 해당 README를 참조한다. 이
프로젝트에서만 발생하는 증상은 다음과 같다.

| 증상 | 원인과 조치 |
|---|---|
| `bootstrap: could not find Conan` | `pipx install conan` 뒤 `pipx ensurepath`를 실행하고 새 terminal을 연다. `pip --user` 설치라면 Python `Scripts`/user `bin` directory를 PATH에 넣고 `conan --version`으로 확인한다 |
| `bind: Address already in use` / `Only one usage of each socket address` | 7301·7302·5083을 다른 process가 사용 중이다 — 이전 실행의 `quickstart_server`·`quickstart_client`가 아직 실행 중일 수 있다 |
| `curl: (7) Failed to connect to 127.0.0.1 port 5083` | client가 아직 뜨지 않았거나 죽었다. `client.log`를 본다 |
| 호출이 대상 없음으로 끝난다 | server가 안 떠 있거나, client의 `peer_connections().connect(...)`가 server의 `listen(...)`과 다른 endpoint를 적었다 |

## 구성

| 디렉터리 | 내용 |
|---|---|
| `Shared/` | 두 process가 함께 컴파일하는 메시지 계약(`hello_t`, `greeting_t`) |
| `Server/` | `greeting` channel handler를 등록하고 `7301`에서 듣는다 |
| `Client/` | server에 연결하고 `GET /hello/{name}`을 열어 `greeting`을 호출한다 |
| `bootstrap.cmake` | 설치 script. tutorial·samples에서도 같은 파일을 사용한다 |
| `CMakePresets.json` | IDE(Visual Studio·Rider·CLion)용 preset. bootstrap이 만든 `.zlink/`를 가리킨다 |

## 내 프로젝트에 옮길 것

- `bootstrap.cmake`, 그리고 `CMakeLists.txt`의 `find_package(zlink_framework CONFIG REQUIRED)`
  + `target_link_libraries(... zlink::framework)` 쌍.
- `Shared/messages.hpp`의 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 메시지 계약 패턴.
- `Server/main.cpp`의 `add_route_mesh(...).listen(...).set_object_role(none).set_routing_id(...)`
  블록 — channel만 쓰는 node에도 모두 필요하다.
- `Client/main.cpp`의 `peer_connections().connect(...)`와 HTTP handler 구성. 실제 서비스는 수동
  연결 대신 Location Store(Redis)를 사용하며, tutorial의 Location 단계가 그 구성을 보인다.
