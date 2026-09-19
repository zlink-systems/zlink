# ZLink Framework .NET 샘플

.NET 샘플은 공개된 11.0.0 framework 계약을 여러 server 역할 process와 실행 가능한
client 시나리오로 보인다. 도메인 흐름과 검증 규칙은
[공용 sample 시나리오](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md)를
따른다.

이 문서는 .NET SDK, Docker, 그리고 (7개 중 6개 샘플에서) Python 3만 있으면 되고
저장소 checkout이 필요 없다. 영어 대응 문서는 [README.md](README.md)다.

## 전제 조건

- **.NET SDK 8.0** — `dotnet --version`이 `8.0.x` SDK를 보고해야 하고 PATH에 있어야
  한다.
- **Docker Desktop**(또는 다른 Docker Engine)이 실행 중이고 샘플을 시작하는 셸에서
  접근 가능해야 한다. `run_sample.sh`/`run_sample.ps1` 각각이 그 실행 전용 Redis
  컨테이너(`redis:7.2-alpine`, 포트 22000–22099)를 스스로 만들고 없앤다 — 따로 Redis를
  설치할 필요가 없고, 실행이 끝나면 컨테이너도 남지 않는다.
- **PowerShell** — `.ps1` 러너는 Windows에 기본 내장된 Windows PowerShell 5.1과
  PowerShell 7 모두에서 동작한다.
- **Python 3**(PATH에 있거나 Windows에서는 `py` launcher로 찾을 수 있어야 한다) —
  단, 일부 샘플·플랫폼에서만 필요하다.
  - Linux·WSL에서는 Bingo·GameQuest·ShoppingMall·SupportChat·TicTacToe·ZoneWorld의
    `run_sample.sh`가 아직 역할 설정 파일을 Python heredoc으로 만든다.
    DeliveryDispatch는 Python이 전혀 필요 없다. 이 zip이 저장소 전용 Python
    helper에 조용히 의존하던 문제(#655)를 없애는 과정에서 포트 선택기와
    ShoppingMall·DeliveryDispatch의 나머지 Python 사용은 이미 제거했고, 남은 설정
    파일 작성 heredoc은 후속 작업으로 남겨 두었다.
  - Windows에서는 ZoneWorld만 Python이 필요하다(`run_sample.ps1`의 ZW-B8 장애
    주입 proxy이며, 기본 `all` 시나리오 실행에 포함된다). 나머지 6개 `.ps1`
    러너는 Python을 전혀 부르지 않는다.
- ZoneWorld의 `-BrowserSmoke`/`--browser-smoke` 플래그는 추가로 Node.js와 npm이
  필요하고 저장소 전체 checkout에서만 동작한다([ZoneWorld](ZoneWorld) 참고).
  기본값이 꺼짐이고 아래 7개 샘플 확인에는 필요 없다.

## 내려받기와 설치

각 샘플은 공개된 `Zlink.Framework`/`Zlink.Stream.Connector` NuGet 패키지만
참조한다(이 디렉터리의 `nuget.config`는 `nuget.org`만 가리킨다). 저장소 checkout이
전혀 필요 없고, 처음 샘플을 빌드할 때 `dotnet build`가 암묵적으로 실행하는
`dotnet restore`가 그 패키지를 내려받는다. `zlink-samples-dotnet.zip`을 아무
디렉터리에나 풀고, 이 문서의 명령을 그 `zlink-samples-dotnet` 디렉터리에서
실행한다(저장소 checkout이면 같은 명령을 `framework/languages/dotnet/samples`에서
실행한다).

## 빌드

`run_sample.sh`/`run_sample.ps1` 각각이 실행 전에 그 샘플을 스스로 빌드하므로
따로 밟을 빌드 단계는 없다. 샘플 하나만 손으로 빌드해 컴파일만 확인하려면:

```bash title="linux"
dotnet build TicTacToe/TicTacToe.sln
```

```powershell title="windows"
dotnet build TicTacToe\TicTacToe.sln
```

## 실행

각 샘플 root가 `run_sample.sh`와 `run_sample.ps1`을 하나씩 갖고, 한 번 실행하면
샘플 하나가 돈다.
[공용 sample 문서](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md)의
"The Sample Run Script And Redis Isolation Standard" 절이 이 규칙을 소유하며,
아래는 이 언어의 명령만 적는다. 이 `samples` 디렉터리(저장소 checkout이면
`framework/languages/dotnet/samples`, `zlink-samples-dotnet.zip`을 풀었으면 그
root)에서 실행한다. 러너가 자기 Redis 컨테이너를 Docker로 직접 띄우므로 따로
손으로 띄우지 않는다. 실행 결과는 다음 절이 읽을 파일에 남긴다.

```bash title="linux"
set -o pipefail
./TicTacToe/run_sample.sh 2>&1 | tee tictactoe-run.log
```

```powershell title="windows"
.\TicTacToe\run_sample.ps1 *>&1 | Tee-Object -FilePath tictactoe-run.log
if ($LASTEXITCODE -ne 0) { throw "run_sample.ps1 failed with exit $LASTEXITCODE" }
```

.NET 샘플은 7개이므로 모두 확인하려면 7번 실행한다. `Bingo`, `DeliveryDispatch`,
`GameQuest`, `ShoppingMall`, `SupportChat`, `TicTacToe`, `ZoneWorld`를(그리고 각자
아래 완료 마커도) 하나씩 바꿔 가며 돌린다.

러너는 역할별 설정 파일을 만들고, 각 역할을 별도 process로 시작하고, 준비될 때까지
기다리고, probe나 client self-check를 실행한 뒤, 자신이 만든 process와 Redis
컨테이너를 없앤다. server 코드는 자기 역할만 시작한다.

## 검증

성공한 실행은 빌드하고 모든 시나리오를 돌리고 모든 역할을 깨끗이 종료한 뒤, 종료
직전 마지막 줄로 그 샘플의 완료 마커를 찍고 exit code `0`으로 끝난다.

| 샘플 | 마커 |
|---|---|
| TicTacToe | `tictactoe-placement=completed` |
| Bingo | `bingo-placement=completed` |
| SupportChat | `supportchat-placement=completed` |
| ShoppingMall | `shoppingmall-placement=completed` |
| DeliveryDispatch | `deliverydispatch-placement=completed` |
| GameQuest | `gamequest-placement=completed` |
| ZoneWorld | `zoneworld=completed` |

TicTacToe라면 위 "실행"에서 만든 `tictactoe-run.log`가
`tictactoe-placement=completed`로 끝나는지 본다.

```bash title="linux"
grep -q 'tictactoe-placement=completed' tictactoe-run.log
```

```powershell title="windows"
if (-not (Select-String -Path tictactoe-run.log -Pattern 'tictactoe-placement=completed' -Quiet)) {
  throw "tictactoe verify failed"
}
```

exit code가 0이 아니거나 `scenario ... FAILED` / `!! ... withheld`로 시작하는 줄이
있으면 그 실행은 통과하지 못한 것이다. 어느 쪽이든 러너는 자신이 만든 process와
Redis 컨테이너를 정리한다.

## 문제 해결

- **Docker가 실행 중이 아니다** — `run_sample.sh`/`run_sample.ps1`이 Docker가
  필요하다는 메시지와 함께 즉시 종료한다. Docker Desktop(또는 Docker Engine)을
  띄우고 다시 실행한다.
- **`Could not find N free ports` / 어떤 역할이 자기 endpoint에 bind하지 못한다** —
  이 머신의 다른 무언가가 샘플의 임시 포트 범위(그 실행의 Redis 컨테이너용
  22000–22099, 역할 endpoint용 22100–23999) 중 하나를 이미 쓰고 있다. 그것을
  닫거나 그냥 다시 실행한다 — 러너는 실행마다 새로 무작위 포트 조합을 고른다.
- **`dotnet`이 호환되는 SDK가 없다고 한다** — .NET 8.0 SDK를 설치한다. 더 최신
  major SDK만으로는 그것이 `8.0.x` 런타임을 함께 담고 있지 않으면 부족하다.
- **Python 3이 필요하다**(Linux·WSL, 또는 Windows의 ZoneWorld) — 위 전제 조건을
  보고 Python 3을 설치한다. `python3`(Linux·WSL) 또는 `python`/`py -3`(Windows)이
  Windows 스토어의 alias stub이 아니라 실제 interpreter로 풀리는지 확인한다.
- 실행이 중간에 끊겨(Ctrl-C, 셸 강제 종료) 자기 정리가 돌지 못한 컨테이너나
  process가 남았다면 — 이 zip이 만드는 컨테이너는 모두
  `zlink-<샘플>-dotnet-redis-*`로 이름 붙는다. `docker rm -f`로 지운다.

## 샘플

| 샘플 | 목적 | Peer topology |
|---|---|---|
| [TicTacToe](TicTacToe) | API 역할 둘과 Play 역할 둘로 방 조회, Actor 턴, 실시간 게임 메시지를 보인다. | 수동 MeshNode peer; Redis 방 route store |
| [Bingo](Bingo) | 세션 admission, Entry·room Spot, Actor binding, 타이머 추첨, bound-session 알림을 보인다. | Redis location store |
| [SupportChat](SupportChat) | API·Support·Session 역할로 대화 소유권, 재연결, idle timeout, 종료 알림을 보인다. | Redis location store |
| [ShoppingMall](ShoppingMall) | Commerce API와 order workflow 역할로 event-sourced 주문, projection, fanout event를 보인다. | Redis location store |
| [DeliveryDispatch](DeliveryDispatch) | Dispatch·courier·tracking·customer gateway 역할로 timeout 재배정과 session push를 보인다. | Redis location store |
| [GameQuest](GameQuest) | Session과 player quest owner 역할로 event-sourced quest 진행과 projection을 보인다. | Redis location store |
| [ZoneWorld](ZoneWorld) | Gateway·ZoneNode·Ops 역할로 Actor 이동, zone Logical Multicast, Node 직접 호출, runtime event, 브라우저 시각화를 보인다. | Redis location store |

TicTacToe만 MeshNode peer를 수동으로 설정한다. 나머지 샘플은 모두 Redis
location store로 Spot·Actor 위치를 찾고 MeshNode peer를 맺는다.

## MeshNode와 Channel 이름

물리 mesh는 process당 하나의 MeshNode를 갖는다. `ChannelName(...)`은 그
MeshNode에 논리적 서비스 소속을 추가할 뿐 다른 ROUTER endpoint를 만들지 않는다.
Node 직접 호출, ChannelName select-one, Spot, Actor, Logical Multicast 연산은
모두 같은 MeshNode를 공유한다. Classic fanout만 별도의 PUB/SUB channel이다.

```csharp
var mesh = options.AddRouteMesh("game")
    .Listen("tcp://0.0.0.0:7300"); // 이 process의 MeshNode endpoint를 만든다.

mesh.ChannelName("orders"); // 다른 ROUTER 없이 논리적 서비스 소속만 추가한다.

options.AddFanoutChannel("events")
    .EnablePublisher("tcp://0.0.0.0:7400"); // Classic fanout은 자기 PUB endpoint를 쓴다.
```

## 설정과 계약

Framework host는 endpoint, Redis, routing ID, timeout, logging 설정을 역할별
설정 파일에서 읽어 `AddZLinkFramework(...)`에 타입 있는 설정으로 넘긴다.
Application 코드는 그 값을 환경 변수에서 직접 읽지 않는다. 독립 client는 알아야
할 외부 endpoint와 시나리오 옵션만 검증된 명령줄 인자나 자기 설정 파일로 받는다.

Shared project는 client와 server 역할이 함께 직렬화하는 message 계약만 담는다.
Server topology와 framework 설정은 `Server/Configuration` 아래에, client와
probe 설정은 각자의 project에 속한다.

TicTacToe를 손으로 실행하려면 역할마다 자기 설정 파일을 준다.

```bash
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-a.json
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-b.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-a.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-b.json
```
