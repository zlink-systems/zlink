# MyGame MORPG 로비와 인스턴스

## 메시지 계약과 연결 종료

1. 공유 DTO에 null 허용과 JSON 표현을 선언합니다.
   - null을 허용하는 필드는 `string?`과 같은 nullable 참조 타입이나 `long?`과 같은 nullable 값 타입으로 선언합니다. 필드 생략을 허용하려면 생성자 매개변수에 기본값을 지정합니다.
   - 64비트 정수는 JSON에서 10진 문자열로 표현합니다. 브라우저 클라이언트도 `"handNumber": "42"`처럼 문자열을 보냅니다. [메시지 모델](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/#12-message-value-rules)

시나리오 하나만 실행하려면 `./run-e2e.sh --scenario party`를 사용합니다. 마지막 request의 응답을 받은 뒤 연결을 닫습니다. send 직후 연결을 닫으면 서버가 메시지를 처리하지 못할 수 있습니다.

플레이어는 Session에 접속해 로비에서 파티를 만들고 Dungeon으로 입장합니다. Entry Spot이 첫 입장을 전달하고 동적 Party User Spot이 명단과 준비 상태를 소유합니다. Dungeon은 입장 명단과 진행 단계를 관리합니다. Framework가 Actor 식별자, Session bind와 relocation을 소유합니다.

```mermaid
flowchart LR
    subgraph playNodes["Play nodes"]
      lobby["Play node (party lobby): Entry and Party"]
      instanceA["Play node A (dungeon): Entry and Dungeon"]
      instanceB["Play node B (dungeon): Entry and Dungeon relocation target"]
    end
    players[Players] -->|STREAM| session["Session node"]
    session -->|relay| lobby
    lobby -->|GetOrCreate| instanceA
    instanceA --- instanceB
    lobby -->|Location / lease| location[Redis]
```

이 템플릿은 참고용입니다. 새 게임을 개발할 때는 새 프로젝트를 만들고, 이 템플릿의 구조와 규칙을 참고해 해당 게임의 노드, Spot, Actor, 메시지를 설계합니다. 템플릿의 이름이나 내용을 바꿔 게임 프로젝트로 사용하지 않습니다.

## 요구 사항과 빌드

Linux 컨테이너를 실행할 수 있는 Docker Compose v2를 설치합니다. Bash 또는 Windows용 PowerShell 7에서 스크립트를 실행합니다. 호스트에서 서버를 실행하거나 디버깅하려면 .NET 8 SDK를 설치합니다. `Directory.Packages.props`는 공개 Framework와 STREAM connector 0.34.1, binding 1.26.0을 사용합니다. NuGet.org에서 복원하며 저장소 checkout 없이 빌드할 수 있습니다.

```bash
./run-e2e.sh --build-only
```

```powershell
.\run-e2e.cmd -BuildOnly
```

Docker 빌드는 `server/MyGame.sln`의 프로젝트를 빌드합니다. 확인 후 이미지 태그를 지우고 재사용 가능한 빌드 계층은 유지합니다.

## 로컬 서버 실행

```bash
./run-server.sh
```

```powershell
.\run-server.cmd
```

서버를 빌드한 뒤 Redis는 Docker에서, 게임 노드는 로컬 프로세스로 시작합니다. `Server ready`에 표시되는 WebSocket 주소를 클라이언트에서 사용합니다. HTTP API가 있는 구성은 API 주소도 표시합니다. Ctrl+C로 노드와 Redis를 종료합니다. 서버가 실행 중이면 두 번째 시작을 거부합니다.

Dungeon을 떠나려면 `LeaveDungeonRequest`를 보내고 `DungeonLeft` 응답을 기다립니다. Dungeon은 퇴장 후 남은 플레이어 수를 `remainingPlayers`에 담습니다. 이 값이 0이면 Session이 같은 세대의 Dungeon Spot을 닫아 응답 전에 슬롯을 반환합니다. 연결이 끊기면 재접속에 사용할 멤버십을 유지합니다.

## 클라이언트 연결

Session에 연결하고 예제의 첫 로비 입장 패킷을 보냅니다. 애플리케이션 Entry와 첫 파티 입장 처리기는 lobby, instanceA, instanceB에 등록합니다. 접속 주소는 `Server ready` 또는 Session의 READY JSON에서 확인합니다. 로컬 주소는 같은 호스트에서 사용할 수 있습니다. 외부 클라이언트에는 접근 가능한 인터페이스와 포트를 제공하며 HTTPS 페이지에는 wss://를 설정합니다. 브라우저의 HTTP 호출에는 접근 가능한 origin과 CORS 정책도 필요합니다.

`Directory.Packages.props`의 Zlink.Framework와 Zlink.Stream.Connector에 고정한 버전을 설치 명령의 `<version>`에 사용합니다.

| 클라이언트 | 설치 | 가이드 |
| --- | --- | --- |
| Unity | Native 및 Editor에서는 NuGetForUnity로 Zlink.Stream.Connector를 복원합니다. WebGL에서는 com.zlink.stream-connector.webgl UPM 패키지를 추가합니다. | [.NET engine integration](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/) |
| Unreal | Plugins/ZLinkStreamConnector에 네이티브 플러그인을 복사하고 ThirdParty/ZLink에 해당 SDK를 패키징합니다. | [C++ engine integration](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/) |
| Godot | .NET에서 Zlink.Stream.Connector를 복원합니다. GDExtension은 호환 godot-cpp와 Framework 사전 빌드를 사용합니다. .NET 예제는 Web 내보내기를 지원하지 않습니다. | [.NET](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/), [C++](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/) |
| Cocos Creator | Web 대상에 npm install @zlink-systems/stream-connector@`<version>`을 사용합니다. | [TypeScript engine integration](https://docs.zlink.systems/node/guide/stream-connector/12-engine-integration/) |
| Axmol | ZLinkStreamConnector adapter를 복사하고 zlink_stream_connector_cpp에 링크합니다. | [C++ engine integration](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/) |
| 웹 브라우저 | 웹 빌드의 package root를 import합니다. 페이지 보안 정책에 맞춰 `ws://` 또는 wss://를 사용합니다. | [TypeScript connector](https://docs.zlink.systems/node/guide/stream-connector/) |
| C++ | C++ Framework 사전 빌드와 find_package(zlink_stream_connector_cpp CONFIG REQUIRED)를 사용합니다. | [C++ connector](https://docs.zlink.systems/cpp/guide/stream-connector/) |
| C#/.NET | dotnet add package Zlink.Stream.Connector --version `<version>`을 실행합니다. | [.NET connector](https://docs.zlink.systems/dotnet/guide/stream-connector/) |
| Java | Maven 또는 Gradle에서 systems.zlink:zlink-stream-connector:`<version>`을 사용합니다. | [Java connector](https://docs.zlink.systems/java/guide/stream-connector/) |
| Kotlin | Java 의존성에 systems.zlink:zlink-framework-kotlin:`<version>`을 추가합니다. | [Kotlin connector](https://docs.zlink.systems/kotlin/guide/stream-connector/) |
| Node | Node.js 22 이상에서 npm install @zlink-systems/stream-connector@`<version>`을 사용합니다. | [Node connector](https://docs.zlink.systems/node/guide/stream-connector/) |

### 연결 코드

별도 .NET 8 콘솔 프로젝트를 만들고 공개 STREAM connector를 추가합니다. 다음 예제는 실행 중인 노드의 주소를 사용합니다.

```csharp
using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
var ct = deadline.Token;
await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri(args[0]),
    DispatchMode = ZlinkStreamDispatchMode.Immediate,
    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
});
var received = new TaskCompletionSource<LobbyView>(TaskCreationOptions.RunContinuationsAsynchronously);
using var view = client.On<LobbyView>((message, _) =>
{
    received.TrySetResult(message.Payload!);
    return ValueTask.CompletedTask;
});
using var rejected = client.On<ActionRejected>((message, _) =>
{
    received.TrySetException(new InvalidOperationException(message.Payload!.ToString()));
    return ValueTask.CompletedTask;
});
using var error = client.OnErrorReceived((message, _) =>
{
    received.TrySetException(new IOException(message.Message));
    return ValueTask.CompletedTask;
});
using var disconnected = client.OnDisconnected((_, _) =>
{
    received.TrySetException(new IOException("Disconnected before LobbyView"));
    return ValueTask.CompletedTask;
});
await client.Connect.Async(ct);
await client.Send(new JoinLobbyRequest("guide-player")).Async(ct);
Console.WriteLine(await received.Task.WaitAsync(ct));
```

## 로컬 e2e

```bash
./run-e2e.sh
./run-e2e.sh --scenario party,reconnect,update
./run-e2e.sh --check-structure
```

```powershell
.\run-e2e.cmd
.\run-e2e.cmd -Scenario party,reconnect,update
.\run-e2e.cmd -CheckStructure
```

실행 중인 서버가 있으면 .logs/server.json을 확인하고 자동 연결합니다. 서버를 종료한 상태에서는 Docker로 격리 실행합니다. 노드 교체가 필요한 update는 연결 실행에서 제외합니다.

| 시나리오 | 검증 내용 |
| --- | --- |
| `party` | 서버가 Party ID와 Run ID를 발급합니다. 준비되지 않거나 리더가 아닌 요청과 내부 패킷은 거부합니다. 연속 입장은 동일한 Dungeon을 사용하며 FinalStage에서 완료합니다. |
| `reconnect` | p2가 기존 Actor, Dungeon, RunId, Phase, Stage로 재접속합니다. |
| `update` | relocation에서 Session, 명단, RunId, Phase, Stage를 유지합니다. 교체 노드가 다른 파티와 Dungeon을 수락합니다. |
| `dungeon-leave` | 한 명이 떠난 뒤 남은 플레이어가 같은 Dungeon에서 다음 단계로 진행하고, 마지막 퇴장으로 슬롯이 반환되는지 확인합니다. |

격리 실행은 독립 Compose 프로젝트를 만들고 READY를 확인한 뒤 시나리오를 실행합니다. docker cp로 설정을 전달하고 docker start -a -i로 노드 표준 입출력을 연결합니다. 각 실행은 자신이 시작한 프로세스와 컨테이너를 정리합니다. 호스트 노드를 디버깅하려면 `--local` 또는 -Local을 사용합니다. Redis는 Docker에서 실행합니다. Debug 빌드가 있으면 `--no-build` 또는 -NoBuild로 빌드를 생략할 수 있습니다.

도메인 검사는 `./run-e2e.sh --check-domain` 또는 .\run-e2e.cmd -CheckDomain으로 실행합니다. 일반 e2e에서도 해당 검사를 실행합니다.

콘솔에 시나리오별 성공·실패와 합계를 표시합니다. .logs/run-<id>/에 노드의 `.out.log`, `.err.log`, .flow.log와 실패 상세 *-failure.log를 기록합니다. Framework dispatch 진단은 `Normal`입니다. 독립 배포를 사용하는 시나리오는 해당 실행 디렉터리 아래 시나리오 디렉터리에 노드 로그를 기록합니다. Docker 빌드와 정리 로그는 .logs/docker-<project>/에 있습니다.

## Framework 동작

- **DB 또는 파일 I/O** 일반 async 호출은 Spot 실행 권한을 유지합니다. Yield는 기다리는 동안 다른 handler의 실행을 허용합니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/#io-worker-wait)
- **생성 기한** 생성 기한 초과와 호출자의 취소는 별도의 결과입니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/22-actor/#creation-deadline) [server/MyGame.Session/PlayerSession.cs:47](server/MyGame.Session/PlayerSession.cs)
- **생성 기한** 생성 기한 초과와 호출자의 취소는 별도의 결과입니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/25-location/#creation-load)
- **송신 부하** 송신은 수용 공간을 기다리며 애플리케이션에는 최종 결과가 전달됩니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/#send-backpressure) [server/MyGame.Instance/DungeonSpot.cs:61](server/MyGame.Instance/DungeonSpot.cs)
- **중복 로그인** 기본 bind는 다른 Session을 받아들입니다. 거부 정책은 Actor callback에서 정합니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/39-session-binding/#duplicate-session-bind)
- **ObjectGeneration** ObjectGeneration은 같은 ID의 논리 인스턴스를 식별합니다. 데이터 버전이나 쓰기 권한은 별도 계약을 따릅니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/#object-generation) [server/MyGame.Session/PlayerSession.cs:65](server/MyGame.Session/PlayerSession.cs)
- **READY와 Serving** Framework가 공개 런타임의 Serving을 확인한 뒤 READY를 출력합니다. [Guide](https://docs.zlink.systems/dotnet/guide/server/12-operations/#wait-for-serving) [server/MyGame.Shared/NodeControl.cs:19](server/MyGame.Shared/NodeControl.cs) [server/MyGame.Shared/NodeControl.cs:40](server/MyGame.Shared/NodeControl.cs)

## 검증 대상 버전

공개 Framework 0.34.1 패키지로 검증합니다.

## 배포 순서

Redis, 파티 로비와 Dungeon play node, Session 순으로 시작합니다. 교체 시 relocation을 받을 수 있는 용량이 남은 Instance가 필요합니다. Party Spot은 Entry에 저장하지 않으며 adapter는 게임 상태를 보관합니다.

server/config/*.json에 접근 가능한 Redis, mesh, STREAM 주소와 배포별 KeyPrefix를 설정합니다. 노드 실행 파일에 --config <경로>를 전달하고 READY를 확인합니다. 교체 대상 표준 입력에 drain을 보내고 RELOCATION_REQUESTED, RELOCATED, SAFE_TO_SHUTDOWN을 확인한 뒤 shutdown을 보내 종료를 기다립니다. 교체 노드는 같은 논리 노드 설정으로 시작합니다. 배포 전에 플레이어 인증과 접근 가능한 listener를 구성합니다. Framework가 배치, bind, membership, 라우팅과 relocation 완료를 관리합니다.

## 알려진 문제

drain 확인에 약 30초가 걸리므로 대기하는 동안 진행 중인 판의 턴 시간이 끝날 수 있습니다. 턴 시계가 있는 게임의 update 시나리오에서는 판을 끝낸 뒤 옮깁니다. 남은 시간이 이전되는지는 시계를 제어하는 도메인 검사에서 확인합니다.
