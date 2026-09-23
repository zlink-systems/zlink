# ZLink Framework .NET 샘플

.NET 샘플은 `Directory.Packages.props`가 고정한 버전의 배포 package `Zlink.Framework`를
여러 server 역할 process와 실행 가능한 client 시나리오로 보여 준다. 도메인 흐름과
검증 규칙은
[공용 sample 시나리오](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md)를
따른다.

이 문서는 .NET SDK와 Docker만 있으면 되고 저장소 checkout이 필요 없다. 영어 대응 문서는
[README.md](README.md)다.

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

- **.NET SDK 8.0** — `dotnet --version`이 `8.0.x` SDK를 보고해야 하고 PATH에 있어야
  한다.
- **Docker Desktop**(또는 다른 Docker Engine)이 실행 중이고 샘플을 시작하는 셸에서
  접근 가능해야 한다. `run_sample.sh`/`run_sample.ps1` 각각이 그 실행 전용 Redis
  컨테이너(`redis:7.2-alpine`, 포트 22000–22099)를 스스로 만들고 없앤다 — 따로 Redis를
  설치할 필요가 없고, 실행이 끝나면 컨테이너도 남지 않는다.
- **PowerShell** — `.ps1` 러너는 Windows에 기본 내장된 Windows PowerShell 5.1과
  PowerShell 7 모두에서 동작한다.
- ZoneWorld의 `--browser-smoke` 플래그는 추가로 Node.js와 npm이
  필요하고 저장소 전체 checkout에서만 동작한다([ZoneWorld](ZoneWorld) 참고).
  기본값은 꺼져 있으며 아래 sample 확인에는 필요 없다.

## 내려받기와 설치

각 샘플은 공개된 `Zlink.Framework`/`Zlink.Stream.Connector` NuGet 패키지만
참조한다(이 디렉터리의 `nuget.config`는 `nuget.org`만 가리킨다). `zlink-dotnet-examples`
저장소를 clone하고 이 문서의 명령을 `samples/`에서 실행한다(저장소 checkout이면 같은
명령을 `framework/languages/dotnet/samples`에서 실행한다). 처음 샘플을 빌드할 때
`dotnet build`가 암묵적으로 실행하는 `dotnet restore`가 그 패키지를 내려받는다.

## 빌드

`run_sample.sh`/`run_sample.ps1` 각각이 실행 전에 그 샘플을 스스로 빌드하므로
별도의 빌드 단계는 없다. sample 하나만 직접 빌드하여 컴파일만 확인하려면 다음 명령을 사용한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet build TicTacToe/TicTacToe.sln
```

**Windows — PowerShell 7**

```powershell title="windows"
dotnet build TicTacToe\TicTacToe.sln
```

## IDE에서 열기

Rider 또는 Visual Studio 2022·2026에서 `Samples.sln`을 열면 샘플 일곱 개의 project가 솔루션 폴더
하나씩으로 나뉘어 열린다. 샘플 하나만 열 때는 `<Sample>/<Sample>.sln`을 연다. 두 솔루션은 같은
project 파일을 가리키므로 어느 쪽에서 빌드해도 결과가 같다.

샘플 시나리오의 실행은 IDE의 시작 project가 아니라 `run_sample.sh`·`run_sample.ps1`로 한다.
러너가 그 실행 전용 Redis 컨테이너를 만들고, 각 server role의 설정 파일과 endpoint를 만들어
같은 project를 여러 instance로 띄우며, client 시나리오를 끝까지 돌린 뒤 모두 정리한다 — 이
값들은 IDE의 `Multiple startup projects`로는 만들 수 없다. IDE에서는 코드를 읽고 고치고 빌드하며,
role 하나를 디버그하려면 러너를 실행한 뒤 그 role의 process에 attach한다(Rider **Run → Attach to
Process**, Visual Studio **디버그 → 프로세스에 연결**; process 이름은 role의 project 이름이다).
종료는 러너가 하며, attach한 디버거는 IDE의 Stop 버튼으로 뗀다.

## 실행

각 샘플 root가 `run_sample.sh`와 `run_sample.ps1`을 하나씩 갖고, 한 번 실행하면
sample 하나가 실행된다.
[공용 sample 문서](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md)의
"The Sample Run Script And Redis Isolation Standard" 절이 이 규칙을 소유하며,
아래는 이 언어의 명령만 적는다. 이 `samples` 디렉터리(저장소 checkout이면
`framework/languages/dotnet/samples`, examples repository를 clone했으면 `samples/`
root)에서 실행한다. 러너가 자기 Redis 컨테이너를 Docker로 직접 띄우므로 따로
손으로 띄우지 않는다. 실행 결과는 다음 절이 읽을 파일에 남긴다.

**Linux · macOS · WSL — bash**

```bash title="linux"
set -o pipefail
./TicTacToe/run_sample.sh 2>&1 | tee tictactoe-run.log
```

**Windows — PowerShell 7**

```powershell title="windows"
.\TicTacToe\run_sample.ps1 *>&1 | Tee-Object -FilePath tictactoe-run.log
if ($LASTEXITCODE -ne 0) { throw "run_sample.ps1 failed with exit $LASTEXITCODE" }
```

모든 sample을 확인하려면 `Bingo`, `DeliveryDispatch`, `GameQuest`,
`ShoppingMall`, `SupportChat`, `TicTacToe`, `ZoneWorld`를 각각 실행하고 아래 완료
marker를 확인한다.

러너는 역할별 설정 파일을 만들고, 각 역할을 별도 process로 시작하고, 준비될 때까지
기다리고, probe나 client self-check를 실행한 뒤, 자신이 만든 process와 Redis
컨테이너를 없앤다. server 코드는 자기 역할만 시작한다.

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

성공한 실행은 빌드와 모든 시나리오를 마치고 모든 역할을 정상 종료한다. 마지막 줄에는 해당
sample의 완료 마커를 기록하고 종료 코드 `0`으로 끝난다.

| 샘플 | 마커 |
|---|---|
| TicTacToe | `tictactoe-placement=completed` |
| Bingo | `bingo-placement=completed` |
| SupportChat | `supportchat-placement=completed` |
| ShoppingMall | `shoppingmall-placement=completed` |
| DeliveryDispatch | `deliverydispatch-placement=completed` |
| GameQuest | `gamequest-placement=completed` |
| ZoneWorld | `zoneworld=completed` |

TicTacToe는 위 "실행"에서 만든 `tictactoe-run.log`의 마지막 줄이
`tictactoe-placement=completed`인지 확인한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
grep -q 'tictactoe-placement=completed' tictactoe-run.log
echo "tictactoe=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
if (-not (Select-String -Path tictactoe-run.log -Pattern 'tictactoe-placement=completed' -Quiet)) {
  throw "tictactoe verify failed"
}
Write-Output 'tictactoe=ok'
```

exit code가 0이 아니거나 `scenario ... FAILED` / `!! ... withheld`로 시작하는 줄이
있으면 그 실행은 통과하지 못한 것이다. 어느 쪽이든 러너는 자신이 만든 process와
Redis 컨테이너를 정리한다.

## 문제 해결

- **Docker가 실행 중이 아니다** — `run_sample.sh`/`run_sample.ps1`이 Docker가
  필요하다는 메시지와 함께 즉시 종료한다. Docker Desktop(또는 Docker Engine)을
  띄우고 다시 실행한다.
- **`Could not find N free ports` / 어떤 역할이 자기 endpoint에 bind하지 못한다** —
  다른 process가 sample의 임시 포트 범위(그 실행의 Redis container용
  22000–22099, 역할 endpoint용 22100–23999)를 이미 사용 중이다. 해당 process를
  종료하거나 다시 실행한다 — runner는 실행마다 새 임의 포트 조합을 선택한다.
- **`dotnet`이 호환되는 SDK가 없다고 한다** — .NET 8.0 SDK를 설치한다. 더 최신
  major SDK만으로는 그것이 `8.0.x` 런타임을 함께 담고 있지 않으면 부족하다.
- 실행이 중간에 끊겨(Ctrl-C, 셸 강제 종료) 자기 정리가 돌지 못한 컨테이너나
  process가 남았다면 — sample runner가 만드는 컨테이너는 모두
  `zlink-<샘플>-dotnet-redis-*`로 이름 붙는다. `docker rm -f`로 지운다.

## 샘플

| 샘플 | 목적 | Peer topology |
|---|---|---|
| [TicTacToe](TicTacToe) | API 역할과 Play 역할로 방 조회, Actor 턴, 실시간 게임 메시지를 보인다. | 수동 MeshNode peer; Redis 방 route store |
| [Bingo](Bingo) | 세션 admission, Entry·room Spot, Actor binding, 타이머 추첨, bound-session 알림을 보인다. | Redis location store |
| [SupportChat](SupportChat) | API·Support·Session 역할로 대화 소유권, 재연결, idle timeout, 종료 알림을 보인다. | Redis location store |
| [ShoppingMall](ShoppingMall) | Commerce API와 order workflow 역할로 event-sourced 주문, projection, fanout event를 보인다. | Redis location store |
| [DeliveryDispatch](DeliveryDispatch) | Dispatch·courier·tracking·customer gateway 역할로 timeout 재배정과 session push를 보인다. | Redis location store |
| [GameQuest](GameQuest) | Session과 player quest owner 역할로 event-sourced quest 진행과 projection을 보인다. | Redis location store |
| [ZoneWorld](ZoneWorld) | Gateway·ZoneNode·Ops 역할로 Actor 이동, zone Logical Multicast, Node 직접 호출, runtime event, 브라우저 시각화를 보인다. | Redis location store |

TicTacToe만 MeshNode peer를 수동으로 설정한다. 나머지 sample은 Redis
Location Store로 Spot·Actor 위치를 찾고 MeshNode peer를 구성한다.

## MeshNode와 Channel 이름

물리 mesh는 process마다 MeshNode 하나를 둔다. `ChannelName(...)`은 MeshNode에 논리적
서비스 소속을 추가할 뿐 별도 ROUTER endpoint를 만들지 않는다.
Node 직접 호출, ChannelName select-one, Spot, Actor, Logical Multicast 연산은
모두 같은 MeshNode를 공유한다. Classic fanout만 별도의 PUB/SUB channel이다.

```csharp
var mesh = options.AddRouteMesh("game")
    .Listen("tcp://127.0.0.1:7300"); // 이 process의 MeshNode endpoint를 만든다.

mesh.ChannelName("orders"); // 다른 ROUTER 없이 논리적 서비스 소속만 추가한다.

options.AddFanoutChannel("events")
    .EnablePublisher("tcp://127.0.0.1:7400"); // Classic fanout은 자기 PUB endpoint를 쓴다.
```

## 설정과 계약

Framework host는 endpoint, Redis, routing ID, timeout, logging 설정을 역할별
설정 파일에서 읽어 `AddZLinkFramework(...)`에 타입 있는 설정으로 전달한다.
Application 코드는 값을 환경 변수에서 직접 읽지 않는다. 독립 client는 필요한
외부 endpoint와 시나리오 옵션만 검증된 명령줄 인자나 설정 파일로 받는다.

Shared 프로젝트는 client와 server 역할이 함께 직렬화하는 message 계약만 둔다.
Server topology와 framework 설정은 `Server/Configuration` 아래에, client와
probe 설정은 각 프로젝트에 둔다.

TicTacToe를 직접 실행할 때는 역할마다 설정 파일을 지정한다.

```bash
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-a.json
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-b.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-a.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-b.json
```
