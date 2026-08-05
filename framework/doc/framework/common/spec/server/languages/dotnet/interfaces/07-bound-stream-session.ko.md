# .NET bound STREAM session 공개 인터페이스

[.NET exact interface 목차](README.ko.md)

## 1. Bound STREAM session

Actor에서 client로 보내는 표면은 one-way send와 disconnect만 제공한다.

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

Client request에 대한 reply는 Actor request handler의 반환값으로 처리한다. Bound session은 client를 향한
새 request operation을 제공하지 않는다.
