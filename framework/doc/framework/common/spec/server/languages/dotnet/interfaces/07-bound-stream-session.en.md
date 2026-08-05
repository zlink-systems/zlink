# .NET Bound STREAM Session Public Interface

[.NET exact interface table of contents](README.en.md)

## 1. Bound STREAM Session

The surface from an Actor to a client only provides one-way send and
disconnect.

```csharp
public interface IZLinkBoundSession
{
    IZLinkBoundSessionSendCall Send<TMessage>(TMessage message);
    ValueTask DisconnectAsync(
        CancellationToken cancellationToken = default);
}

public interface IZLinkBoundSessionSendCall
    : IZLinkMetadataCall<IZLinkBoundSessionSendCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}
```

The reply to a client request is handled by the Actor request handler's
return value. Bound session doesn't provide a new request operation
toward the client.
