[← 목차](README.ko.md)

# 4. Request 만들기

## HTTP 메서드

```kotlin
client.get("/players/7281")
client.post("/games")
client.put("/games/42")
client.delete("/games/42")
client.patch("/games/42")
client.head("/games/42")
client.options("/games")
```

path는 반드시 `/`로 시작한다. `baseUrl`에 경로 prefix가 있으면 prefix와 결합된다
(예: base `http://h/api` + path `/games` → `/api/games`).

## query 파라미터

```kotlin
client.get("/search").query("q", "ranked match").query("limit", "20").awaitRaw()
// → /search?q=ranked%20match&limit=20
```

## 헤더

```kotlin
client.get("/players/7281").header("x-trace-id", "abc-123").awaitRaw()
```

client 기본 헤더(`defaultHeader`)와 합쳐지며, 같은 이름이면 요청별 값이 우선한다.

## 요청별 timeout

```kotlin
client.get("/slow").timeout(Duration.ofSeconds(10)).awaitRaw()
```

[다음: Request Body →](05-request-body.ko.md)
