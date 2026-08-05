[← Table Of Contents](README.en.md)

# 7. Async

`AsyncRaw()` / `Async<T>()` / `DownloadAsync(sink)` return a `ValueTask<T>`. For a server call that
doesn't need the response result, use the argument-less `Async()`. It returns no normal completion
value or transport status, delivering only a pre-start failure as an asynchronous error. The HTTP
request builder has no `Yield<T>()` that gives back the Spot turn.

## Non-Blocking Guarantee

An HTTP call completes asynchronously. The completion value is never pulled out with a blocking API
to wait for the response.

```csharp
public async ValueTask NotifyMatchResultAsync(ZLinkHttpClient client, MatchResult result)
{
    var ack = await client.Post($"/matches/{result.MatchId}/result")
        .Body(result)
        .Fetch<AckRes>();

    if (!ack.Accepted)
    {
        // The HTTP call succeeded, but the application rejected the result.
        throw new InvalidOperationException("match result was not accepted");
    }
}
```

If you don't need the response, use the one-way terminator on the server client injected via DI.

```csharp
await client.Post($"/matches/{result.MatchId}/events")
    .Body(result)
    .Async(); // No normal completion value — this only starts request execution.
```

## Choosing The Spot Turn

`Fetch<T>()`/`Async<T>()` keep the current Spot turn. While waiting for the response, the same
Spot's next callback doesn't start, so use this when you need to preserve state invariants across the
request.

To give back the shared Spot gate while waiting for an external HTTP response, run `Async<T>()`
inside `RunIoWorker(...)` and wait on the worker call's `Yield()`. In the continuation after
re-acquiring the gate, re-check state, since another callback may have changed Spot state.

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
        .Yield(); // The worker call gives back the gate, not the HTTP client.
}
```

There's no public terminator that synchronously pulls out the completion value. Blocking unwrapping
like `.GetAwaiter().GetResult()` is also not used in a framework handler.

| Call Site | Recommended |
|-----------|------|
| A state-preserving request in a framework handler | `await Async<T>()` |
| Independent external I/O with gate release in a framework handler | `await Async<T>()` inside `RunIoWorker(...).Yield()` |
| Tests, client scenarios, CLI, batch | `await Async<T>()` |

## Callback Completion

The callback overload doesn't return an awaitable. Called from a Spot handler, it returns without
occupying the current execution line, and the completion callback is handled as a new turn on the
same execution line.

```csharp
client.Get("/health").Async<HealthRes>((error, response) =>
{
    if (error is not null) return; // Confirm transport/status/decode failures.
    RecordHealth(response!.Body);  // This callback runs on a separate Spot turn.
});
```

## Streaming Callback Location

`DownloadAsync(sink)`'s sink is called in the asynchronous context reading the response chunk.
Don't block the thread with heavy synchronous work inside the sink — hand it off to a thread-safe
queue if needed.

[Next: Streaming →](08-streaming.en.md)
