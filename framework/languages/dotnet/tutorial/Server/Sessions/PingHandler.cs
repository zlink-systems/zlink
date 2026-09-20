using Tutorial.Shared;
using Zlink.Framework.Contracts.Streams;

namespace Tutorial.Server.Sessions;

// --8<-- [start:session-handler]
// The first type argument is the session context, not the session class.
public sealed class PingHandler : IZLinkSessionPacketHandler<IZLinkSessionContext, Ping>
{
    public ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Ping message,
        CancellationToken cancellationToken
    )
        // Reply answers a request. To push to a client that is not waiting for
        // one, use Client.Send instead.
        =>
        context.Client.Reply(new Pong(message.SentAtUnixMs)).Async(cancellationToken);
}
// --8<-- [end:session-handler]
