[← 목차](README.ko.md)

# 7. 비동기

`submitRaw()` / `submit<T>()` / `download(sink)`는 모두 `Promise`를 돌려준다. Node에서는
`async`/`await`가 코루틴 역할을 한다.

## non-blocking 보장

undici는 libuv event loop 기반 비동기 소켓을 쓴다. 따라서 응답을 기다리는 동안
**event loop 스레드는 막히지 않는다.** 런타임의 비동기 I/O가 이를 제공하므로 별도의
worker scheduler가 필요 없다.

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

> DNS 해석(`getaddrinfo`)만 OS 레벨에서 blocking이지만 Node는 이를 백그라운드 스레드
> 풀로 offload하므로 event loop는 막히지 않는다.

## continuation 재개 위치

Node는 단일 event loop라 continuation 재개 위치 주입 개념이 없다. `.coroutines()` 빌더 항목은
존재하지 않으며 표준 `Promise`/`await`만 제공한다.

## blocking 경로 없음

Node에는 동기 blocking HTTP 접근이 없다. blocking 제출 경로를 제공하지 않으며 모든
제출은 `await`한다.

[다음: Streaming →](08-streaming.ko.md)
