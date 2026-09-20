using Tutorial.Shared;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace Tutorial.Server.Sessions;

// --8<-- [start:session-class]
// One connected game client. Callbacks for the same connection run in order.
public sealed class GameSession(IZLinkSessionContext context, ILogger<GameSession> logger)
    : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    // Session handlers are not discovered by scanning, and the packet name must
    // match what the client sends. Without this, no packet is ever handled.
    public void Configure()
    {
        Context.Handlers.AddHandler<PingHandler>(nameof(Ping));
        Context.Handlers.AddHandler<AuthenticateHandler>(nameof(Authenticate));
    }

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        logger.LogInformation("client connected: {SessionId}", Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        logger.LogInformation("client disconnected: {SessionId}", Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        logger.LogWarning("stream error on {SessionId}: {Error}", Context.SessionId, error);
        return ValueTask.CompletedTask;
    }

    // Every inbound packet arrives here first. Registering a handler is not
    // enough on its own; this method is what routes the packet to it.
    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
            return;

        // --8<-- [start:session-actor-relay]
        // Anything without a session handler is forwarded to the player bound to
        // this connection, which is why authentication has to come first.
        var bound = Context.Actors.Bound;
        if (bound.Count != 1)
            throw new InvalidOperationException("Authenticate before sending player packets.");

        await bound.Single().RelayAsync(payload, cancellationToken);
        // --8<-- [end:session-actor-relay]
    }
}
// --8<-- [end:session-class]
