namespace Zlink.Framework.Contracts.Streams;

public interface IZLinkBoundSession
{
    IZLinkBoundSessionSendCall Send<TMessage>(
        TMessage message);

    ValueTask DisconnectAsync(
        CancellationToken cancellationToken = default);
}

public interface IZLinkBoundSessionSendCall
    : IZLinkMetadataCall<IZLinkBoundSessionSendCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}
