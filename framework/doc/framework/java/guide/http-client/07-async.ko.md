[← 목차](README.ko.md)

# 7. 비동기

`submitRaw()` / `submit(Type)` / `download(sink)`는 모두 `CompletionStage`를 돌려준다.

## non-blocking 보장

`java.net.http.HttpClient.sendAsync`는 NIO selector 기반 비동기 I/O를 쓴다. 따라서 응답을
기다리는 동안 **호출 스레드는 park되지 않는다.** 런타임의 비동기 I/O가 이를 제공하므로
별도의 worker scheduler가 필요 없다.
래퍼의 redirect 루프·retry 루프도 `CompletionStage` 체인으로 합성되어 hop 사이에 스레드를
점유하지 않는다(블로킹 본문 read만 executor로 offload).

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

> DNS 해석(`getaddrinfo`)만 OS 레벨에서 blocking이지만 JDK는 이를 HttpClient executor로
> offload하므로 호출 스레드는 막히지 않는다.

## handler 규칙 — `.get()`/`.join()` 금지

> **framework handler 스레드에서는 `CompletionStage` 합성(`thenCompose`/`thenApply`/
> `thenAccept`)만 쓰고 `.get()`/`.join()`은 쓰지 않는다.** 이 repo의 java
> `await`/`join`은 blocking이라 handler 스레드를 막는다.

| 호출 위치 | 권장 |
|-----------|------|
| framework handler / actor / spot 코드 | `submit(Type).thenCompose(...)` |
| 테스트 코드 | `fetch(Type)` 또는 `.toCompletableFuture().join()` |
| client 시나리오·CLI·배치 | `fetch(Type)` |

## continuation 재개 위치

Java에서는 `CompletableFuture`의 `*Async(fn, executor)` 조합으로 continuation 재개 위치를 제어한다. continuation을 특정 executor에서 재개하려면 `thenApplyAsync`/`thenComposeAsync`에
executor를 넘긴다.

## blocking: fetch(Type)

`fetch(Type)`는 결과가 올 때까지 호출 스레드를 멈추고 typed body를 돌려주며 실패를 예외로
던진다. 테스트·CLI 전용이다.

```java
Leaderboard board = client.get("/leaderboard").fetch(Leaderboard.class);
```

[다음: Streaming →](08-streaming.ko.md)
