[← Table Of Contents](README.en.md)

# 5. Request Body

Body sources are **mutually exclusive**. You can use only one of `Body`, `BodyStream`, `Form`, or
`Multipart` — specifying two or more fails with `ProtocolError`.

## Typed JSON DTO

```csharp
await client.Post("/games")
    .Body(new CreateGameReq("ranked-match-0611"))
    .Fetch<CreateGameRes>();
```

Unless a separate codec extension is registered, `Body<T>(dto)` serializes the DTO with the Web
default (`JsonSerializerDefaults.Web`) and sets `content-type: application/json`. You don't need to
register a codec per message type.

## Raw Body

```csharp
await client.Post("/raw").Body("plain text payload", "text/plain").AsyncRaw();
```

## Form-Urlencoded

```csharp
await client.Post("/login")
    .Form("user", "aria")
    .Form("password", "secret value")
    .AsyncRaw();
// content-type: application/x-www-form-urlencoded, percent-encoding applied
```

## Multipart/Form-Data

```csharp
await client.Post("/upload")
    .Multipart("title", "patch notes")
    .MultipartFile("file", "notes.txt", fileContent, "text/plain")
    .AsyncRaw();
```

## Streaming Upload

`BodyStream(provider, contentType)` sends the body chunk by chunk with chunked
transfer-encoding. The provider returns `null` when done. A streaming request can't be rewound, so
it's **excluded from automatic retry**.

```csharp
var chunks = new Queue<byte[]>(/* ... */);
await client.Post("/upload-stream")
    .BodyStream(
        () => chunks.Count > 0 ? chunks.Dequeue() : null,
        "application/octet-stream")
    .AsyncRaw();
```

The provider is a `Func<byte[]?>` — it returns `null` when done (being a reference type, `? chunk :
null` works cleanly).

[Next: Handling Responses →](06-handling-responses.en.md)
