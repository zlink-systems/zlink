[← 목차](README.ko.md)

# 5. Request Body

body 소스는 **상호 배타**다. `body`, `bodyStream`, `form`, `multipart` 중 하나만 쓸 수
있고 둘 이상 지정하면 `requestProtocolError`로 실패한다.

## typed JSON

```ts
await client.post('/games').body({ name: 'ranked-match-0611' }).submit<CreateGameRes>();
```

`body(value)`(1 인자)는 값을 JSON 직렬화하고 `content-type: application/json`을 설정한다.

## raw body

```ts
await client.post('/raw').body('plain text payload', 'text/plain').submitRaw();
```

`body(content, contentType)`(2 인자)는 raw 본문과 명시적 content-type을 설정한다.

## form-urlencoded

```ts
await client.post('/login').form('user', 'aria').form('password', 'secret value').submitRaw();
// content-type: application/x-www-form-urlencoded, percent-encoding 적용
```

## multipart/form-data

```ts
await client.post('/upload')
  .multipart('title', 'patch notes')
  .multipartFile('file', 'notes.txt', fileContent, 'text/plain')
  .submitRaw();
```

## streaming 업로드

`bodyStream(provider, contentType)`는 body를 chunk 단위로 chunked transfer-encoding으로
전송한다. provider는 `() => Uint8Array | null` 타입이며 끝나면 `null`을 돌려준다.
streaming 요청은 rewind할 수 없으므로 **자동 retry에서 제외**된다.

```ts
const chunks: Uint8Array[] = [new TextEncoder().encode('payload')];
let i = 0;
await client.post('/upload-stream')
  .bodyStream(() => (i < chunks.length ? chunks[i++] : null), 'application/octet-stream')
  .submitRaw();
```

[다음: Response 다루기 →](06-handling-responses.ko.md)
