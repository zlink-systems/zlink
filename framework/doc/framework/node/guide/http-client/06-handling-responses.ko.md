[← 목차](README.ko.md)

# 6. Response 다루기

## raw 응답

`submitRaw()`는 `RawHttpResponse`를 돌려준다.

```ts
const response = await client.get('/players/7281').submitRaw();
const status = response.status;
const body = response.body;                 // string
const contentType = response.headers['content-type'];
```

응답 헤더 이름은 소문자다.

## typed JSON 응답

`submit<T>()`는 응답을 JSON으로 디코드해 `HttpResponse<T>`를 돌려준다.

```ts
const response = await client.get('/players/7281').submit<PlayerProfile>();
const profile = response.body;     // 디코드된 객체
const raw = response.rawBody;      // 원본 응답 텍스트
```

- status가 **400 이상**이면 `ZLinkFrameworkException(requestFailed)`를 던진다.
- 본문 JSON 디코드 실패는 `ZLinkFrameworkException(payloadDecodeFailed)`로 보고된다.
- JSON 파싱은 prototype-pollution(`__proto__`/`constructor`/`prototype`) 방어를 적용한다.

## status 처리 정리

| 경로 | 4xx/5xx |
|------|---------|
| `submitRaw()` | status를 그대로 돌려준다(예외 없음) |
| `submit<T>()` | `requestFailed` 예외 |
| `fetch<T>()` | `submit<T>()`와 같이 검증하고 디코드한 body만 `Promise<T>`로 반환 |

```ts
const profile = await client.get('/players/7281').fetch<PlayerProfile>();
// response wrapper가 필요 없을 때 body만 받는 비동기 편의 표면이다.
```

[다음: 비동기 →](07-async.ko.md)
