[English](./README.md) | [한국어](./README.ko.md)

# ZLink C++ Framework Samples

C++ 샘플은 framework의 공개 API로 여러 server 역할을 구성하는 방법을 보여 준다.
업무 흐름과 검증 기준은 공통 sample 문서(`framework/doc/framework/common/sample/README.ko.md`)를
따르며, C++ 코드는 runtime reflection 대신 compile-time 타입으로 handler를 등록한다.

각 서버 실행 파일은 자기 역할만 구성한다. Runner는 역할별 프로세스를 시작하고 연결 준비를
확인한 뒤 공개 client 시나리오를 실행하며, 종료할 때 자신이 시작한 프로세스와 Redis
container를 정리한다. 샘플 코드가 다른 서버 역할을 같은 프로세스에서 시작하지 않는다.

이 디렉터리는 `zlink-cpp-examples` 저장소의 `samples/`이다. 아래 절차는 GitHub Release에
공개된 Core·binding·framework 패키지와 Conan, 그리고 Redis를 띄울 Docker를 사용한다.

## 차례

- [전제 조건](#전제-조건)
- [내려받기와 설치](#내려받기와-설치)
- [빌드](#빌드)
- [실행](#실행)
- [검증](#검증)
- [문제 해결](#문제-해결)
- [샘플 목록](#샘플-목록)
- [설정과 계약 배치](#설정과-계약-배치)

`빌드`·`실행`·`검증` 절의 명령 블록은 `title="linux"`(bash)와 `title="windows"`(PowerShell)로
표시되어 있다. 각 블록은 `zlink-cpp-examples` 저장소를 clone한 뒤 `samples/`에서 그대로
실행되며, 릴리스 CI가 같은 블록을 그대로 돌린다.

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

| 도구 | Windows | Linux · WSL |
|---|---|---|
| C++20 컴파일러 | Visual Studio 2022 17.4 이상 또는 Visual Studio 2026, **Desktop development with C++** 워크로드. 2026-09 현재 2026의 msvc 195에는 ConanCenter 바이너리가 없어 첫 bootstrap이 서드파티 library를 source에서 빌드하므로 약 20분 걸린다. 2022는 바이너리를 내려받아 약 7분 걸린다 (2022는 MSVC 19.44로 확인) | GCC 13 이상 (13.3으로 확인) |
| CMake | 3.24 이상 (Visual Studio가 설치하는 3.31로 확인) | 3.24 이상 (3.28로 확인) |
| Ninja | 필요 없음 | 권장. 없으면 Makefile로 빌드한다 |
| Conan 2 | `pipx install conan` 뒤 `pipx ensurepath`를 실행하고 새 terminal을 연다. `py -m pip install --user conan`을 썼으면 Python `Scripts` directory를 PATH에 넣는다. `conan --version`으로 확인한다 | `pipx install conan` 뒤 `pipx ensurepath`를 실행하고 새 terminal을 연다. `python3 -m pip install --user conan`을 썼으면 Python user `bin` directory를 PATH에 넣는다. `conan --version`으로 확인한다 |
| Docker Desktop | runner가 샘플마다 Redis container를 하나 띄운다. 설치되어 실행 중이어야 한다 | 같다 (WSL integration 또는 Linux의 Docker Engine) |
| `curl` | Windows 10 이상에 들어 있다 | 배포판 패키지 |

이 밖에는 아무것도 필요 없다. zlink 저장소, Node.js는 쓰지 않는다. ZoneWorld의 ZW-B8 장애
proxy까지 C++로 샘플과 함께 빌드된다. Conan은 pipx(또는 pip)로 설치하며 ConanCenter의 세 번째
파티 바이너리를 받는다. Visual Studio 2022에서는 **첫 bootstrap에 약 7분**, 2026의 msvc 195처럼
바이너리가 없는 toolset에서는 서드파티를 source에서 빌드하므로 **약 20분** 걸린다. 이후에는
로컬 캐시를 재사용한다.

## 내려받기와 설치

[`zlink-cpp-examples`](https://github.com/zlink-systems/zlink-cpp-examples) 저장소를 clone한다.
아래 명령은 모두 그 저장소의 `samples/` 안에서 실행한다.

설치는 [빌드](#빌드) 블록 첫 줄의 `bootstrap.cmake`로 처리한다. GitHub Release에서
아카이브 — 이 플랫폼의 Core prebuilt(`core/v1.2.0`), C++ binding 소스(`cpp/v1.2.0`), framework
소스(`framework-cpp/v0.18.0`) — 를 받아 binding과 framework를 빌드해 `.zlink/install/`에
설치하고, 모든 sample을 한 프로젝트로 `build/`에 구성한다. 기본 package manager는 Conan이며
vcpkg fallback은 `-DZLINK_PACKAGE_MANAGER=vcpkg`로 선택한다. 이후 실행에서는 받은 파일과 빌드
결과를 그대로 사용한다.

병렬도는 논리 코어 수가 기본이며 `cmake -DZLINK_JOBS=4 -P bootstrap.cmake`처럼 `-P` 앞에
두어 줄인다. 다시 처음부터 하려면 `.zlink/`와 `build/`를 지운다.

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

역할별 실행 파일 28개와 ZoneWorld proxy는 Windows의 `build\Release\`, Linux의
`build/` 아래에 생성된다. Windows에서는 Core `zlink.dll`과 서드파티 DLL이 실행 파일 옆에 함께
복사된다. Linux runner는 실행 전에 자기 샘플의 target을 다시 빌드하므로 한 샘플만 볼 때는
`cmake --build`를 건너뛰어도 된다.

## 실행

샘플마다 `run_sample.ps1`과 `run_sample.sh`가 있고, 한 번의 호출은 샘플 하나를 실행한다.
**Runner가 Redis를 Docker container로 직접 띄우고**(`redis:7-alpine`, `127.0.0.1`의
20000–20099 중 빈 port)를 실행이 끝날 때 제거한다. Redis를 미리 시작할 필요는 없다. 아래 블록은
각 sample을 차례로 실행한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./Bingo/run_sample.sh
./DeliveryDispatch/run_sample.sh
./GameQuest/run_sample.sh
./ShoppingMall/run_sample.sh
./SupportChat/run_sample.sh
./TicTacToe/run_sample.sh
./ZoneWorld/run_sample.sh
```

**Windows — PowerShell 7**

```powershell title="windows"
.\Bingo\run_sample.ps1
.\DeliveryDispatch\run_sample.ps1
.\GameQuest\run_sample.ps1
.\ShoppingMall\run_sample.ps1
.\SupportChat\run_sample.ps1
.\TicTacToe\run_sample.ps1
.\ZoneWorld\run_sample.ps1
```

각 호출은 sample 하나를 실행한다. Runner는 빌드, 역할별 설정 파일 생성, server 시작, readiness 확인,
client self-check와 정리를 순서대로 수행한다. application 포트는 `127.0.0.1`의 20100–21999
안에서 실행마다 새로 고른다.

`ZLINK_CPP_BUILD_DIR`을 지정하면 `build/` 대신 그 빌드 트리의 실행 파일을 쓴다.

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

Runner의 마지막 줄이 아래 표의 표식이고 종료 코드가 0이면 그 샘플은 통과다. 표식 앞에는
client self-check가 확인한 항목들이 `<샘플>-…=verified` 꼴로 찍힌다.

| 샘플 | 마지막 줄 |
|---|---|
| Bingo | `bingo-placement=completed` |
| DeliveryDispatch | `deliverydispatch-placement=completed` |
| GameQuest | `gamequest-placement=completed` |
| ShoppingMall | `shoppingmall-placement=completed` |
| SupportChat | `supportchat-placement=completed` |
| TicTacToe | `tictactoe-placement=completed` |
| ZoneWorld | `zoneworld=completed` |

아래 블록은 TicTacToe 하나로 이를 확인한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
./TicTacToe/run_sample.sh | tee tictactoe.log | tail -n 1 | grep -x 'tictactoe-placement=completed'
echo "tictactoe=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
$lines = @(& .\TicTacToe\run_sample.ps1 *>&1 | ForEach-Object { "$_" })
if ($lines[-1] -ne 'tictactoe-placement=completed') { throw ('TicTacToe failed: ' + $lines[-1]) }
Write-Output $lines[-1]
Write-Output 'tictactoe=ok'
```

실패한 실행은 역할별 stdout·stderr 로그가 든 run 디렉터리를 남기고 그 경로를
`<샘플> run directory preserved: …`로 알린다. Linux runner는 framework 자체 테스트 단계를
`framework tests: skipped (package tree; no framework test targets)`로 표시한다 — 그 테스트는
저장소 트리에만 있다.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| `bootstrap: could not find Conan` | `pipx install conan` 뒤 `pipx ensurepath`를 실행하고 새 terminal을 연다. `pip --user` 설치라면 Python `Scripts`/user `bin` directory를 PATH에 넣고 `conan --version`으로 확인한다 |
| `ERROR: Invalid setting ...` | 선택한 컴파일러가 ConanCenter의 지원 바이너리 구성과 다르다. 표의 컴파일러 버전을 쓰거나 `-DZLINK_PACKAGE_MANAGER=vcpkg`를 지정한다 |
| `bootstrap: download failed: https://github.com/...` | GitHub Release에 닿지 못했다. 프록시·방화벽을 확인하고 다시 실행한다 |
| `CMake Error ... No CMAKE_CXX_COMPILER could be found` / `Visual Studio 17 2022 could not find any instance` | 컴파일러가 없다. Windows는 **Desktop development with C++** 워크로드, Linux는 `g++`를 설치한다 |
| `No configured build tree at .../build.` (Linux) / `Missing executable: ... Build C++ samples first or set ZLINK_CPP_BUILD_DIR.` (Windows) | 설치나 빌드를 건너뛰었다. `cmake -P bootstrap.cmake`, Windows는 이어서 `cmake --build build --config Release`를 실행한다 |
| Windows에서 역할 process가 즉시 끝나고 로그가 비어 있다. runner가 `Timed out waiting for <역할>`로 끝난다 (종료 코드 `-1073741515`, `STATUS_DLL_NOT_FOUND`) | `zlink.dll`이 실행 파일 옆에 없다. `cmake --build build --config Release`를 다시 실행하면 post-build 단계가 `build\Release\`에 복사한다 |
| `Docker is required to run the <샘플> sample.` / `docker: error during connect` / `Cannot connect to the Docker daemon` | Docker Desktop이 실행 중이 아니다. 띄운 뒤 다시 실행한다 |
| `Failed to create Redis container ...` 안에 `port is already allocated` | 20000–20099가 모두 사용 중이다. 이전 실행이 남긴 `zlink-redis-cpp-sample-*` container를 `docker ps`로 찾아 지운다 |
| `<샘플> sample startup port collision; retrying with fresh ports` | 다른 process가 고른 port를 먼저 잡았다. Runner가 새 port로 최대 3번 다시 시도하므로 조치가 필요 없다 |
| `Timed out waiting for <역할> at tcp://127.0.0.1:<port>` | 그 역할이 뜨지 못했다. 보존된 run 디렉터리의 `<역할>.trace.log`(또는 `<역할>.log`)를 본다 |
| Windows에서 `run_sample.ps1 cannot be loaded because running scripts is disabled` | 실행 정책이다. `powershell -ExecutionPolicy Bypass -File .\TicTacToe\run_sample.ps1`로 실행한다 |

## 샘플 목록

| 샘플 | 보여 주는 기능 | 연결 구성 | payload codec |
|---|---|---|---|
| `Bingo` | Session gateway, Entry Spot, room Spot, Actor binding, timer와 bound-session push | Redis location store | Protobuf |
| `TicTacToe` | API 2개와 Play 2개의 scale-out, room route 조회와 실시간 게임 | 수동 peer endpoint와 Redis room route store | JSON |
| `SupportChat` | conversation Spot, 상담 배정, reconnect, idle timer와 종료 알림 | Redis location store | JSON |
| `DeliveryDispatch` | courier 선택, timeout 재배정, tracking과 customer push | Redis location store | JSON |
| `GameQuest` | player별 quest owner Spot, event stream과 조회 모델 | Redis location store | JSON |
| `ShoppingMall` | ChannelName service, 주문 workflow, event stream과 fanout 알림 | Redis location store | JSON |
| `ZoneWorld` | Gateway와 ZoneNode 2개, Ops를 분리해 zone 이동, actor relocation, border sync와 운영 fanout을 확인한다 | Redis location store | JSON |

TicTacToe만 peer endpoint를 수동으로 설정한다. 다른 샘플은 Spot과 Actor의 위치를 찾고 MeshNode
peer를 구성할 때 Redis location store를 사용한다. Application code가 peer 목록이나 연결 순서를
관리하지 않는다.

`samples/TicTacToe/run_sample.sh`와 `samples/Bingo/run_sample.sh`는 모든 server 역할과 공개
client self-check를 함께 실행한다. 저장소 tree에서는 그 전에
framework 자체 테스트도 함께 돈다.

하나의 물리 mesh는 process마다 MeshNode 하나로 구성한다. `ChannelName`은 그 MeshNode가
참여하는 논리 service group이며 별도 ROUTER endpoint를 만들지 않는다. Node direct, ChannelName
select-one, Spot, Actor와 Logical Multicast는 같은 MeshNode를 사용한다. 모든 수신자에게
전달하는 classic fanout은 별도 PUB/SUB channel이다.

## 설정과 계약 배치

서버 역할은 설정 파일 경로를 받고, `app.config()`가 읽은 값을 typed configuration에 bind한 뒤
framework builder에 전달한다. Endpoint, Redis, routing ID, timeout과 로그 경로를 application
환경 변수로 직접 읽지 않는다. Standalone client는 직접 연결해야 하는 외부 endpoint와
timeout만 검증된 CLI option 또는 client 설정 파일로 받는다.

`Shared/Contracts`에는 client와 server가 함께 직렬화하는 message 계약만 둔다. 서버 topology,
ChannelName, endpoint 이름, packet 이름과 timing 설정은 `Server/Configuration`에 두고, client
전용 설정은 `Client/Configuration`에 둔다.

수동으로 역할 하나를 실행할 때도 역할별 설정 파일을 넘긴다.

```bash
sample_cpp_framework_tictactoe_play --config=./appsettings.play-a.json
sample_cpp_framework_tictactoe_api --config=./appsettings.api-a.json
sample_cpp_framework_tictactoe_client --api-http-endpoint=http://127.0.0.1:48113
```

각 샘플의 `CMakeLists.txt`는 단독으로도 구성된다 — `find_package(zlink_framework CONFIG
REQUIRED)`에 `CMAKE_PREFIX_PATH`로 `.zlink/install`을 주면 된다. 루트의 `CMakeLists.txt`는
모든 sample을 하나의 빌드 트리에 모은다.
