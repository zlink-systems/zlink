[← 목차](README.ko.md)

# 7. 비동기

`AsyncRaw()` / `Async<T>()` / `DownloadAsync(sink)`는 `ValueTask<T>`를 돌려준다.
응답 결과가 필요 없는 server 호출에는 인자 없는 `Async()`를 사용한다. 정상 완료 값과
전송 상태는 반환하지 않으며, 시작 전 실패만 비동기 오류로 전달한다. HTTP request builder에는
Spot turn을 반납하는 `Yield<T>()`가 없다.

## non-blocking 보장

HTTP 호출은 비동기로 완료된다. 응답을 기다리기 위해 blocking API로 완료 값을 꺼내지 않는다.

```csharp
public async ValueTask NotifyMatchResultAsync(ZLinkHttpClient client, MatchResult result)
{
    var ack = await client.Post($"/matches/{result.MatchId}/result")
        .Body(result)
        .Fetch<AckRes>();

    if (!ack.Accepted)
    {
        // HTTP 호출은 성공했지만 application이 결과를 거부했다.
        throw new InvalidOperationException("match result was not accepted");
    }
}
```

응답이 필요 없다면 DI로 주입받은 server client에서 one-way terminal을 사용한다.

```csharp
await client.Post($"/matches/{result.MatchId}/events")
    .Body(result)
    .Async(); // 정상 완료 값은 없으며 request 실행만 시작한다.
```

## Spot turn 선택

`Fetch<T>()`·`Async<T>()`는 현재 Spot turn을 유지한다. 응답을 기다리는 동안 같은 Spot의 다음 callback이
시작하지 않으므로, 요청 전후의 상태 불변식을 유지해야 할 때 사용한다.

외부 HTTP 응답을 기다리는 동안 shared Spot gate를 반납하려면 `RunIoWorker(...)` 안에서
`Async<T>()`를 실행하고 worker call의 `Yield()`로 기다린다. Gate를 다시 얻은 continuation에서는 다른
callback이 Spot 상태를 바꿨을 수 있으므로 상태를 다시 확인한다.

```csharp
public async ValueTask<PlayerProfile> LoadProfileAsync(
    IZLinkSpotContext context,
    ZLinkHttpServerClient client,
    string playerId)
{
    return await context
        .RunIoWorker(async workerCancellation =>
            await client.Get($"/players/{playerId}")
                .Fetch<PlayerProfile>(workerCancellation))
        .Yield(); // Gate 반납은 HTTP client가 아니라 worker call이 수행한다.
}
```

완료 값을 동기로 꺼내는 public terminator는 없다. `.GetAwaiter().GetResult()` 같은 blocking
언래핑도 framework handler에서 사용하지 않는다.

| 호출 위치 | 권장 |
|-----------|------|
| framework handler의 상태 보존 요청 | `await Async<T>()` |
| framework handler의 독립된 외부 I/O와 gate 반납 | `RunIoWorker(...).Yield()` 안에서 `await Async<T>()` |
| 테스트·client 시나리오·CLI·배치 | `await Async<T>()` |

## callback 완료

callback overload는 awaitable을 돌려주지 않는다. Spot handler에서 호출하면 현재 실행 줄을
점유하지 않고 반환하며, 완료 callback은 같은 실행 줄의 새 turn으로 처리된다.

```csharp
client.Get("/health").Async<HealthRes>((error, response) =>
{
    if (error is not null) return; // 전송·status·decode 실패를 확인한다.
    RecordHealth(response!.Body);  // 이 callback은 별도 Spot turn에서 실행된다.
});
```

## streaming callback 위치

`DownloadAsync(sink)`의 sink는 응답 chunk를 읽는 비동기 컨텍스트에서 호출된다. sink
안에서 무거운 동기 작업으로 스레드를 막지 말고 필요하면 thread-safe queue로 넘긴다.

[다음: Streaming →](08-streaming.ko.md)
