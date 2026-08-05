[← Table Of Contents](README.en.md)

# 7. Async

`submitRaw()` / `submit<T>()` / `download(sink)` all return a `Promise`. In Node, `async`/`await`
plays the role of a coroutine.

## Non-Blocking Guarantee

undici uses asynchronous sockets based on the libuv event loop. So while waiting for a response,
**the event loop thread is not blocked.** The runtime's asynchronous I/O provides this, so no
separate worker scheduler is needed.

```ts
async function notifyMatchResult(client: ZLinkHttpClient, result: MatchResult): Promise<void> {
  const response = await client.post(`/matches/${result.matchId}/result`)
    .body(result)
    .submit<AckRes>();

  if (!response.body.accepted) {
    throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.RequestFailed, 'match result was not accepted');
  }
}
```

> Only DNS resolution (`getaddrinfo`) is blocking at the OS level, but Node offloads it to a
> background thread pool, so the event loop isn't blocked.

## Continuation Resume Location

Node has a single event loop, so there's no concept of injecting a continuation resume location. The
`.coroutines()` builder item doesn't exist — only standard `Promise`/`await` are provided.

## No Blocking Path

Node has no synchronous blocking HTTP access. It provides no blocking submit path — every submit is
`await`ed.

[Next: Streaming →](08-streaming.en.md)
