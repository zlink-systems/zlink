# .NET Bound STREAM Session Public Interface

[.NET per-language interface table of contents](README.en.md)

## 1. Bound STREAM Session

[ZLinkSessionBindingIdentity, defined in Actor interface §1](06-actors.en.md#1-actor), exposes the binding
identity from §4 as a read-only value. The Actor's `OnDuplicateBindAsync` receives the current and
incoming values in this form.

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
    ValueTask Async();
}
```

The reply to a client request is handled by the Actor request handler's
return value. Bound session doesn't provide a new request operation
toward the client.
