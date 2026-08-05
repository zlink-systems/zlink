[← Table Of Contents](README.en.md)

# 7. Coroutines

`awaitRaw()` / `await(type)` / `await<T>()` / `fetch<T>()` / `awaitDownload(sink)` are all `suspend`
functions.

## Non-Blocking Guarantee

The internal transport uses native asynchronous I/O, and the `suspend` extension bridges its
`CompletionStage` with `kotlinx-coroutines-jdk8`'s `await()`. So **no thread is parked** while
waiting for a response. The redirect loop and retry loop also don't occupy a thread between hops.

```kotlin
suspend fun notifyMatchResult(client: ZLinkHttpClient, result: MatchResult) {
    val ack = client.post("/matches/${result.matchId}/result").body(result).await<AckRes>()
    if (!ack.body().accepted) {
        throw ZLinkFrameworkException("match result was not accepted")
    }
}
```

> Only DNS resolution (`getaddrinfo`) is blocking at the OS level, but it's offloaded to the
> transport executor, so the calling thread isn't blocked.

## In A Handler — Compose With Suspend

Framework handler/actor/spot code calls `await`/`fetch` directly inside a suspend function, composing
sequentially. `runBlocking` blocks the handler thread, so it's not used there.

| Call Site | Recommended |
|-----------|------|
| framework handler / actor / spot code | `await`/`fetch` inside a suspend function |
| test · CLI · `main` | `runBlocking { ... }` |

## Concurrency

Launching several requests with `async` and `awaitAll` doesn't serialize them even on a single
dispatcher (native asynchronous I/O).

```kotlin
val results = (1..20).map { async { client.get("/r").awaitRaw() } }.awaitAll()
```

## Resume Dispatcher

`await()` resumes on the calling coroutine's `CoroutineDispatcher`. To change the resume location,
wrap it with `withContext(dispatcher) { ... }`.

```kotlin
val report = withContext(Dispatchers.IO) {
    client.get("/reports/summary").fetch<Report>()
}
```

[Next: Streaming →](08-streaming.en.md)
