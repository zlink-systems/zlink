[← 목차](README.ko.md)

# 7. 코루틴

`awaitRaw()` / `await(type)` / `await<T>()` / `fetch<T>()` / `awaitDownload(sink)`는 모두
`suspend` 함수다.

## non-blocking 보장

내부 전송은 네이티브 비동기 I/O를 쓰고 `suspend` 확장이 그 `CompletionStage`를
`kotlinx-coroutines-jdk8`의 `await()`로 잇는다. 따라서 응답을 기다리는 동안 **어떤 스레드도
park되지 않는다.** redirect 루프·retry 루프도 hop 사이에 스레드를 점유하지 않는다.

```kotlin
suspend fun notifyMatchResult(client: ZLinkHttpClient, result: MatchResult) {
    val ack = client.post("/matches/${result.matchId}/result").body(result).await<AckRes>()
    if (!ack.body().accepted) {
        throw ZLinkFrameworkException("match result was not accepted")
    }
}
```

> DNS 해석(`getaddrinfo`)만 OS 레벨에서 blocking이지만 transport executor로 offload되므로
> 호출 스레드는 막히지 않는다.

## handler에서 — suspend로 합성

framework handler·actor·spot 코드는 suspend 함수 안에서 `await`/`fetch`를 직접 호출해
순차로 합성한다. `runBlocking`은 handler 스레드를 막으므로 쓰지 않는다.

| 호출 위치 | 권장 |
|-----------|------|
| framework handler / actor / spot 코드 | suspend 함수 안에서 `await`/`fetch` |
| 테스트 · CLI · `main` | `runBlocking { ... }` |

## 동시성

여러 요청을 `async`로 띄우고 `awaitAll`하면 단일 디스패처에서도 직렬화되지 않는다(네이티브
비동기 I/O).

```kotlin
val results = (1..20).map { async { client.get("/r").awaitRaw() } }.awaitAll()
```

## resume dispatcher

`await()`는 호출한 coroutine의 `CoroutineDispatcher`에서 재개된다. 재개 위치를 바꾸려면
`withContext(dispatcher) { ... }`로 감싼다.

```kotlin
val report = withContext(Dispatchers.IO) {
    client.get("/reports/summary").fetch<Report>()
}
```

[다음: Streaming →](08-streaming.ko.md)
