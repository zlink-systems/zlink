[← 목차](README.ko.md)

# 4. Request 만들기

## HTTP 메서드

```csharp
client.Get("/players/7281");
client.Post("/games");
client.Put("/games/42");
client.Delete("/games/42");
client.Patch("/games/42");
client.Head("/games/42");
client.Options("/games");
```

path는 반드시 `/`로 시작한다. `BaseUrl`에 경로 prefix가 있으면 prefix와 결합된다
(예: base `http://h/api` + path `/games` → `/api/games`).

## query 파라미터

`Query(name, value)`는 percent-encoding된 query 파라미터를 추가한다.

```csharp
await client.Get("/search")
    .Query("q", "ranked match")
    .Query("limit", "20")
    .AsyncRaw();
// → /search?q=ranked%20match&limit=20
```

## 헤더

요청별 헤더는 `Header(name, value)`로 추가한다. client 기본 헤더(`DefaultHeader`)와
합쳐지며, 같은 이름이면 요청별 값이 우선한다.

```csharp
await client.Get("/players/7281")
    .Header("x-trace-id", "abc-123")
    .AsyncRaw();
```

## 요청별 timeout

```csharp
await client.Get("/slow").Timeout(TimeSpan.FromSeconds(10)).AsyncRaw();
```

[다음: Request Body →](05-request-body.ko.md)
