[← 목차](README.ko.md)

# 2. 시작하기

## 프로젝트 참조

소비 프로젝트에 `Zlink.HttpClient` package를 추가한다. 서버 runtime 전체가 아니라
HTTP client가 사용하는 framework contract package만 함께 설치된다.

```xml
<ItemGroup>
  <PackageReference Include="Zlink.HttpClient" Version="0.5.1" />
</ItemGroup>
```

```csharp
using Zlink.HttpClient;
```

## 첫 요청

```csharp
using var client = ZLinkHttpClient.Create("http://127.0.0.1:18080")
    .Build();

var player = await client.Get("/players/7281").Fetch<PlayerProfile>();
Console.WriteLine(player.Name);
```

- `Create(baseUrl)`로 builder를 시작하고 `.Build()`로 client를 만든다.
- client는 재사용 가능하고 thread-safe하다. 보통 한 번 만들어 오래 쓴다.
- `using`으로 client 수명을 관리한다.

## 한 줄 요청

단발 요청은 `Build()`를 생략하고 builder에서 바로 메서드를 호출할 수 있다.

```csharp
var res = await ZLinkHttpClient.Create("https://game-api.example.internal")
    .Post("/games")
    .Body(new CreateGameReq("ranked-match-0611"))
    .Fetch<CreateGameRes>();
```

반복 호출한다면 client를 한 번 만들어 재사용하는 편이 connection pool 재사용 측면에서
유리하다.

## callback으로 완료 받기

```csharp
client.Get("/leaderboard").Async<Leaderboard>((error, response) =>
{
    if (error is not null)
    {
        Console.Error.WriteLine(error.Message); // 실패도 같은 callback에서 확인한다.
        return;
    }

    Console.WriteLine(response!.Body.TopPlayer); // 성공 응답의 typed body를 사용한다.
});
```

callback은 awaitable을 사용하지 않는 client나 이벤트 루프 코드에 적합하다. 완료 값을 이어서
계산해야 하는 일반 코드는 `Async<T>()`를 `await`한다([7장](07-async.ko.md)).

[다음: Client 구성 →](03-client-configuration.ko.md)
