[← Table Of Contents](README.en.md)

# 2. Getting Started

## Dependency

```kotlin
// build.gradle.kts
dependencies {
    implementation(project(":zlink-http-client-kotlin"))
}
```

Pulls in `kotlinx-coroutines-jdk8` and `jackson-module-kotlin` (`data class` DTO serialization) as
transitive dependencies.

```kotlin
import systems.zlink.httpclient.kotlin.*
import systems.zlink.httpclient.ZLinkHttpClient
```

## First Request

```kotlin
zlinkHttpClient("http://127.0.0.1:18080").use { client ->
    val player = client.get("/players/7281").await<PlayerProfile>()
    println(player.body().name)
}
```

- `zlinkHttpClient(baseUrl) { ... }` applies the DSL block to a fluent builder to create the client.
- The client is reusable and thread-safe. Since it's `AutoCloseable`, managing its lifetime with
  `use { ... }` cleans up the internal connection pool and executor.
- `await<T>()` is a reified extension that suspends all the way to a typed JSON response.

## DTO

`data class`es are used directly as request/response bodies.

```kotlin
data class CreateGameReq(val mode: String)
data class CreateGameRes(val id: String, val ranked: Boolean)
```

## One-Line Request

```kotlin
suspend fun createGame(): CreateGameRes =
    zlinkHttpClient("https://game-api.example.internal").use { client ->
        client.post("/games").body(CreateGameReq("ranked-match-0611")).fetch()
    }
```

`fetch<T>()` is a suspend extension that directly returns the typed body (a convenience for
`await<T>().body()`).

## Test · CLI

Outside a coroutine (tests, `main`), wrap the call in `runBlocking`.

```kotlin
fun main() = runBlocking {
    zlinkHttpClient("http://127.0.0.1:18080").use { client ->
        val board = client.get("/leaderboard").fetch<Leaderboard>()
        println(board)
    }
}
```

[Next: Client Configuration →](03-client-configuration.en.md)
