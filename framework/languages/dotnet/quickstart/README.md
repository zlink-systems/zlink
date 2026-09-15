# ZLink .NET quickstart

`framework/doc/framework/dotnet/guide/server/02-getting-started.ko.md` §2 "최소 예제 —
두 process가 서로 호출한다"와 같은 시나리오를 실제로 빌드·실행되는 프로젝트로 둔 것이다.
Redis도 location store도 없이, endpoint를 직접 적는 수동 연결로 request/reply 하나를
돌린다. 이 저장소 밖에서도 nuget.org의 게시 패키지만으로 빌드된다.

## 전제

- .NET SDK **8.0 이상** (`net8.0` target). 확인한 SDK: `dotnet --version` → `8.0.131`.
- 인터넷 접속(nuget.org). `nuget.config`가 nuget.org만 본다.
- Redis나 다른 외부 의존은 필요 없다.

## 구성

- `Shared/` — `Hello`, `Greeting` record. 두 process가 참조하는 계약.
- `Server/` — `greeting` channel을 처리하는 process. `tcp://0.0.0.0:7101`에서 듣는다.
- `Client/` — `greeting`을 호출하고 `GET /hello/{name}`을 노출하는 process.
  `tcp://0.0.0.0:7102`에서 듣고, server의 `tcp://127.0.0.1:7101`에 수동으로 연결한다.
- `Directory.Packages.props` — `Zlink.Framework`·`Zlink.Framework.AspNetCore` 버전을
  중앙에서 명시적 숫자로 고정한다(조건 분기 없음). binding `Zlink`는 고정하지 않고
  `Zlink.Framework`의 전이 의존에 맡긴다.

## 실행 명령

```bash
cd framework/languages/dotnet/quickstart
dotnet build

# 터미널 두 개(또는 백그라운드)로 각각 띄운다.
dotnet run --project Server/Server.csproj
dotnet run --project Client/Client.csproj

# server가 뜬 뒤 client를 띄우고 나서 호출한다.
curl http://127.0.0.1:5080/hello/world
```

## 기대 출력

```
$ curl http://127.0.0.1:5080/hello/world
"hello, world"
```

HTTP 200. client 로그에 `Executed endpoint 'HTTP: GET /hello/{name}'`과
`Request finished ... - 200 -`가 남는다.

## 가이드 §2 본문과 다른 점 (실제로 돌리기 위해 필요했다)

가이드 §2는 서술을 최소화한 스니펫이다. 그대로 옮기면 아래 두 지점에서 막힌다. 문서
자체는 이 담당(Job G) 밖이라 고치지 않았고, 실행되는 프로젝트만 다음처럼 보정했다.

`framework 0.11.0`(binding `Zlink 0.17.6` 의존) 시절에는 최신 binding `Zlink 1.1.0`과
짝지으면 mesh peer admission(Hello/Admit handshake)이 끝나지 않고 30초 뒤
`ZLinkFrameworkException: Channel 'greeting' did not become selectable before its
deadline.`로 실패해서 `Directory.Packages.props`에 `Zlink 0.17.6`을 손으로 고정해야
했다. **`0.12.0`부터는 이 보정이 필요 없다** — `Zlink.Framework 0.12.0`과
`Zlink.Framework.AspNetCore 0.12.0`의 `.nuspec`이 `Zlink 1.1.0`을 직접 지정하므로
(nuget.org에서 직접 다운로드해 확인), binding 버전은 이제 전이 의존에 맡긴다.
`Directory.Packages.props`와 두 `.csproj`의 `Zlink` 항목을 지웠다.

1. **핸들러 등록.** §2는 `options.AddHandlersFromAssemblyOf<Program>();`로 `HelloHandler`가
   자동 등록되는 것처럼 쓰여 있지만, 이 auto-registration은 attribute 기반 handler
   (`[ZLinkRequestAttribute]` 등)만 mesh channel에 연결한다. `IZLinkRequestHandler<,>`를
   구현하는 class는 channel builder에 명시적으로 등록해야 한다 — 같은 가이드 §8의
   ClientServer channel 예제가 쓰는 것과 같은 방식이다:
   `mesh.Channel("greeting").Server().AddRequestHandler<HelloHandler, Hello, Greeting>();`
   이 quickstart의 `Server/Program.cs`는 이 형태를 쓴다.
2. **HTTP 포트.** 가이드는 두 process 모두 `WebApplication.CreateBuilder(args)`를 쓰지만
   포트를 명시하지 않는다. 같은 호스트에서 두 process를 동시에 띄우면 Kestrel 기본 포트가
   겹친다. `Client`는 `http://127.0.0.1:5080`(가이드의 curl 예제와 같다), `Server`는
   `http://127.0.0.1:5081`을 쓴다 — server 쪽 HTTP는 이 예제에서 실제로 쓰이지 않는다.

## 내 프로젝트에 넣을 때 옮겨야 하는 것

- `Directory.Packages.props`의 두 `PackageVersion` 항목(버전 고정 방식과 실제 값). binding
  `Zlink`는 고정하지 않고 `Zlink.Framework`의 전이 의존에 맡긴다.
- `Shared/Contracts.cs`의 계약 정의 방식(`record` 기반 메시지).
- `Server/Program.cs`의 `AddZLinkFramework` 블록 — mesh 이름, `Listen`, `Channel(...).Server()
  .AddRequestHandler<...>()` 순서.
- `Client/Program.cs`의 `AddZLinkFramework` 블록 — `Channel(...).Client()`,
  `PeerConnections.Connect(...)`, 그리고 `IZLinkRouteClient.RequestToChannel(...).Async<T>()`
  호출부.
- 실제 서비스에서는 수동 `PeerConnections.Connect` 대신 location store(Redis 등)로 옮겨가는
  것이 보통이다 — 이 quickstart는 "설치가 끝났다"만 확인하는 단계라 의도적으로 뺐다.
