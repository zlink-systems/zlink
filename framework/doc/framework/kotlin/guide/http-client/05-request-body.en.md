[← Table Of Contents](README.en.md)

# 5. Request Body

Body sources are **mutually exclusive**. You can use only one of `body`, `bodyStream`, `form`, or
`multipart` — specifying two or more fails with an exception.

## Typed JSON

```kotlin
client.post("/games").body(CreateGameReq("ranked-match-0611")).await<CreateGameRes>()
```

`body(value)` serializes the value with Jackson and sets `content-type: application/json`. A
`data class` is serialized as-is.

## Raw Body

```kotlin
client.post("/raw").body("plain text payload", "text/plain").awaitRaw()
```

`body(content, contentType)` (2 arguments) sets a raw body and an explicit content-type.

## Form-Urlencoded

```kotlin
client.post("/login").form("user", "aria").form("password", "secret value").awaitRaw()
// content-type: application/x-www-form-urlencoded, percent-encoding applied
```

## Multipart/Form-Data

```kotlin
client.post("/upload")
    .multipart("title", "patch notes")
    .multipartFile("file", "notes.txt", fileContent, "text/plain")
    .awaitRaw()
```

## Streaming Upload

`bodyStream(provider, contentType)` sends the body chunk by chunk with chunked transfer-encoding.
It ends when the provider returns `null`. A streaming request can't be rewound, so it's **excluded
from automatic retry**.

```kotlin
val chunks = ArrayDeque(listOf("a".toByteArray(), "b".toByteArray()))
client.post("/upload-stream")
    .bodyStream({ if (chunks.isEmpty()) null else chunks.poll() }, "application/octet-stream")
    .awaitRaw()
```

[Next: Handling Responses →](06-handling-responses.en.md)
