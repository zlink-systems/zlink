[← Table Of Contents](README.en.md)

# 7. Async

`submitRaw()` / `submit(Type)` / `download(sink)` all return a `CompletionStage`.

## Non-Blocking Guarantee

`java.net.http.HttpClient.sendAsync` uses NIO-selector-based asynchronous I/O. So while waiting for
a response, **the calling thread is not parked.** The runtime's asynchronous I/O provides this, so
no separate worker scheduler is needed. The wrapper's redirect loop and retry loop are also composed
as a `CompletionStage` chain, not occupying a thread between hops (only the blocking body read is
offloaded to an executor).

```java
public CompletionStage<Void> notifyMatchResult(ZLinkHttpClient client, MatchResult result) {
    return client.post("/matches/" + result.matchId() + "/result")
        .body(result)
        .submit(AckRes.class)
        .thenAccept(response -> {
            if (!response.body().accepted()) {
                throw new ZLinkFrameworkException("match result was not accepted");
            }
        });
}
```

> Only DNS resolution (`getaddrinfo`) is blocking at the OS level, but the JDK offloads it to the
> HttpClient executor, so the calling thread isn't blocked.

## Handler Rule — No `.get()`/`.join()`

> **On a framework handler thread, use only `CompletionStage` composition (`thenCompose`/
> `thenApply`/`thenAccept`) — don't use `.get()`/`.join()`.** In this repo, Java's `await`/`join` is
> blocking and would block the handler thread.

| Call Site | Recommended |
|-----------|------|
| framework handler / actor / spot code | `submit(Type).thenCompose(...)` |
| test code | `fetch(Type)` or `.toCompletableFuture().join()` |
| client scenario/CLI/batch | `fetch(Type)` |

## Continuation Resume Location

In Java, the continuation resume location is controlled through `CompletableFuture`'s
`*Async(fn, executor)` combinators. To resume a continuation on a specific executor, pass the
executor to `thenApplyAsync`/`thenComposeAsync`.

## Blocking: fetch(Type)

`fetch(Type)` stops the calling thread until the result arrives, returning the typed body and
throwing failures as an exception. It's for test/CLI use only.

```java
Leaderboard board = client.get("/leaderboard").fetch(Leaderboard.class);
```

[Next: Streaming →](08-streaming.en.md)
