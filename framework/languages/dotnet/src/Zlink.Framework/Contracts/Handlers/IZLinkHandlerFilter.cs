namespace Zlink.Framework.Contracts.Handlers;

public enum ZLinkHandlerDispatchKind
{
    NodeDirectSend = 0,
    NodeDirectRequest = 1,
    ChannelSend = 2,
    ChannelRequest = 3,
    ClassicFanout = 4
}

public interface IZLinkHandlerFilterContext : IZLinkMessageContext
{
    ZLinkHandlerDispatchKind DispatchKind { get; }
}

public delegate ValueTask ZLinkHandlerFilterNext();

public interface IZLinkHandlerFilter
{
    ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken);
}
