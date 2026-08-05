[← 목차](README.ko.md)

# 2. 시작하기

## 의존성

```kotlin
// build.gradle.kts
dependencies {
    implementation(project(":zlink-http-client-kotlin"))
}
```

`kotlinx-coroutines-jdk8`과 `jackson-module-kotlin`(`data class` DTO 직렬화)을 전이 의존으로
가져온다.

```kotlin
import systems.zlink.httpclient.kotlin.*
import systems.zlink.httpclient.ZLinkHttpClient
```

## 첫 요청

```kotlin
zlinkHttpClient("http://127.0.0.1:18080").use { client ->
    val player = client.get("/players/7281").await<PlayerProfile>()
    println(player.body().name)
}
```

- `zlinkHttpClient(baseUrl) { ... }`는 DSL 블록을 fluent builder에 적용해 client를 만든다.
- client는 재사용 가능하고 thread-safe하다. `AutoCloseable`이므로 `use { ... }`로 수명을
  관리하면 내부 connection pool과 executor가 정리된다.
- `await<T>()`는 typed JSON 응답까지 suspend하는 reified 확장이다.

## DTO

`data class`를 그대로 요청·응답 본문으로 쓴다.

```kotlin
data class CreateGameReq(val mode: String)
data class CreateGameRes(val id: String, val ranked: Boolean)
```

## 한 줄 요청

```kotlin
suspend fun createGame(): CreateGameRes =
    zlinkHttpClient("https://game-api.example.internal").use { client ->
        client.post("/games").body(CreateGameReq("ranked-match-0611")).fetch()
    }
```

`fetch<T>()`는 typed body를 직접 돌려주는 suspend 확장이다(`await<T>().body()` 편의).

## 테스트 · CLI

coroutine 밖(테스트·`main`)에서는 `runBlocking`으로 감싼다.

```kotlin
fun main() = runBlocking {
    zlinkHttpClient("http://127.0.0.1:18080").use { client ->
        val board = client.get("/leaderboard").fetch<Leaderboard>()
        println(board)
    }
}
```

[다음: Client 구성 →](03-client-configuration.ko.md)
