[← Table Of Contents](README.en.md)

# 5. Request Body

Body sources are **mutually exclusive**. You can use only one of `body`, `bodyStream`, `form`, or
`multipart` — specifying two or more fails with an exception.

## Typed JSON

```java
client.post("/games").body(new CreateGameReq("ranked-match-0611")).submit(CreateGameRes.class);
```

`body(Object)` serializes the value with Jackson and sets `content-type: application/json`.

## Raw Body

```java
client.post("/raw").body("plain text payload", "text/plain").submitRaw();
```

`body(String content, String contentType)` (2 arguments) sets a raw body and an explicit
content-type.

## Form-Urlencoded

```java
client.post("/login").form("user", "aria").form("password", "secret value").submitRaw();
// content-type: application/x-www-form-urlencoded, percent-encoding applied
```

## Multipart/Form-Data

```java
client.post("/upload")
    .multipart("title", "patch notes")
    .multipartFile("file", "notes.txt", fileContent, "text/plain")
    .submitRaw();
```

## Streaming Upload

`bodyStream(provider, contentType)` sends the body chunk by chunk with chunked transfer-encoding.
The provider has type `Supplier<byte[]>`, returning `null` when done. A streaming request can't be
rewound, so it's **excluded from automatic retry**.

```java
Deque<byte[]> chunks = /* ... */;
client.post("/upload-stream")
    .bodyStream(() -> chunks.isEmpty() ? null : chunks.poll(), "application/octet-stream")
    .submitRaw();
```

[Next: Handling Responses →](06-handling-responses.en.md)
