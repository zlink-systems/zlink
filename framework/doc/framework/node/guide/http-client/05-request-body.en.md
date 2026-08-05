[← Table Of Contents](README.en.md)

# 5. Request Body

Body sources are **mutually exclusive**. You can use only one of `body`, `bodyStream`, `form`, or
`multipart` — specifying two or more fails with `requestProtocolError`.

## Typed JSON

```ts
await client.post('/games').body({ name: 'ranked-match-0611' }).submit<CreateGameRes>();
```

`body(value)` (1 argument) JSON-serializes the value and sets `content-type: application/json`.

## Raw Body

```ts
await client.post('/raw').body('plain text payload', 'text/plain').submitRaw();
```

`body(content, contentType)` (2 arguments) sets a raw body and an explicit content-type.

## Form-Urlencoded

```ts
await client.post('/login').form('user', 'aria').form('password', 'secret value').submitRaw();
// content-type: application/x-www-form-urlencoded, percent-encoding applied
```

## Multipart/Form-Data

```ts
await client.post('/upload')
  .multipart('title', 'patch notes')
  .multipartFile('file', 'notes.txt', fileContent, 'text/plain')
  .submitRaw();
```

## Streaming Upload

`bodyStream(provider, contentType)` sends the body chunk by chunk with chunked transfer-encoding.
The provider has type `() => Uint8Array | null`, returning `null` when done. A streaming request
can't be rewound, so it's **excluded from automatic retry**.

```ts
const chunks: Uint8Array[] = [new TextEncoder().encode('payload')];
let i = 0;
await client.post('/upload-stream')
  .bodyStream(() => (i < chunks.length ? chunks[i++] : null), 'application/octet-stream')
  .submitRaw();
```

[Next: Handling Responses →](06-handling-responses.en.md)
