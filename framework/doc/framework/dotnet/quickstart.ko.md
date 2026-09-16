# .NET Quickstart — 빈 프로젝트에서 첫 요청까지

> **이 장의 계약 소유 문서** — 없다. API의 정식 계약은
> [.NET 스펙](../common/spec/server/languages/dotnet/README.ko.md)이 다룬다.

저장소의 [`framework/languages/dotnet/quickstart/`](../../../languages/dotnet/quickstart/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다.

location store 없이 process 둘이 서로의 endpoint를 직접 지정해 request/reply 한 번을
주고받는다. 다음 단계는 [설치와 첫 동작](guide/server/02-getting-started.ko.md)이다.

## 전제

- .NET SDK 8.0 이상 (`net8.0`)
- nuget.org 접근

## 1. 패키지 버전

`Zlink`(binding)는 적지 않는다. `Zlink.Framework`가 의존 버전을 선언한다.

```xml title="Directory.Packages.props"
--8<-- "framework/languages/dotnet/quickstart/Directory.Packages.props"
```

```xml title="nuget.config"
--8<-- "framework/languages/dotnet/quickstart/nuget.config"
```

## 2. 공유 계약

```csharp title="Shared/Contracts.cs"
--8<-- "framework/languages/dotnet/quickstart/Shared/Contracts.cs"
```

## 3. 처리하는 쪽

`AddHandlersFromAssemblyOf`는 handler 타입을 찾기만 한다. 어느 channel에 노출할지는
`Channel(...).Server()`에 따로 등록한다.

```csharp title="Server/Program.cs"
--8<-- "framework/languages/dotnet/quickstart/Server/Program.cs"
```

```xml title="Server/Server.csproj"
--8<-- "framework/languages/dotnet/quickstart/Server/Server.csproj"
```

## 4. 호출하는 쪽

두 process를 같은 host에서 실행하므로 HTTP 포트를 각각 지정한다. 지정하지 않으면 Kestrel
기본 포트가 충돌한다.

```csharp title="Client/Program.cs"
--8<-- "framework/languages/dotnet/quickstart/Client/Program.cs"
```

## 5. 실행

```bash
cd framework/languages/dotnet/quickstart
dotnet build

# 터미널 두 개. server를 먼저 실행한다.
dotnet run --project Server/Server.csproj
dotnet run --project Client/Client.csproj

curl http://127.0.0.1:5080/hello/world
```

응답은 `"hello, world"`, 상태 코드 200이다.

## 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `Directory.Packages.props` | `PackageVersion` 두 항목. binding은 전이 의존으로 둔다 |
| `Shared/Contracts.cs` | `record` 기반 메시지 정의 |
| `Server/Program.cs` | `AddRouteMesh` → `Listen` → `Channel(...).Server().AddRequestHandler<...>()` |
| `Client/Program.cs` | `Channel(...).Client()`, `PeerConnections.Connect(...)`, `RequestToChannel(...).Async<T>()` |

수동 `PeerConnections.Connect` 대신 location store를 쓰는 구성은
[10. Location](guide/server/10-location.ko.md)이 다룬다.
