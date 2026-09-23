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
        // A slotted packet uses its dispatch Actor. An unslotted packet can use
        // the connection only while exactly one Actor is bound.
        var actor =
            dispatch.Actor
            ?? (
                Context.Actors.Bound.Count switch
                {
                    1 => Context.Actors.Bound.Single(),
                    0 => throw new InvalidOperationException(
                        "Authenticate an Actor before sending player packets."
                    ),
                    _ => throw new InvalidOperationException(
                        "Select an Actor handle when more than one Actor is bound."
                    ),
                }
            );

        await actor.RelayAsync(payload, cancellationToken);
        // --8<-- [end:session-actor-relay]
    }
}
// --8<-- [end:session-class]
