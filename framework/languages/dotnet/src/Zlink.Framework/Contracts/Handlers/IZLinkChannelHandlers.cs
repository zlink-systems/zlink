namespace Zlink.Framework.Contracts.Handlers;

public interface IZLinkRequestHandler<in TRequest, TResponse>
{
    ValueTask<TResponse> HandleAsync(
        TRequest request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkSendHandler<in TMessage>
{
    ValueTask HandleAsync(
        TMessage message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkFanoutHandler<in TEvent>
{
    ValueTask HandleAsync(
        TEvent message,
        CancellationToken cancellationToken);
}
