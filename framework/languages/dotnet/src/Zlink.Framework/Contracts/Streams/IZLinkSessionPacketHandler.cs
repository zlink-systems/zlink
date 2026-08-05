namespace Zlink.Framework.Contracts.Streams;

public interface IZLinkSessionHandlerRegistry
{
    void AddHandler<THandler>()
        where THandler : class;

    void AddHandler<THandler>(string packetName)
        where THandler : class;

    ValueTask<bool> TryHandleAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSessionPacketHandler<in TSessionContext, TMessage>
{
    ValueTask HandleAsync(
        TSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        TMessage message,
        CancellationToken cancellationToken);
}
