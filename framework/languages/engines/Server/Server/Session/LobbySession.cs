using EngineLobby.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace EngineLobby.Server;

public sealed class LobbySession(IZLinkSessionContext context, ILogger<LobbySession> logger)
    : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<PingHandler>(nameof(PingReq));
        Context.Handlers.AddHandler<JoinHandler>(nameof(JoinReq));
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

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
            return;

        var bound = Context.Actors.Bound;
        if (bound.Count != 1)
            throw new InvalidOperationException("JoinReq before sending lobby packets.");

        await bound.Single().RelayAsync(payload, cancellationToken);
    }
}

public sealed class PingHandler : IZLinkSessionPacketHandler<IZLinkSessionContext, PingReq>
{
    public ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        PingReq message,
        CancellationToken cancellationToken
    ) => context.Client.Reply(new PingRes(message.SentAtUnixMs)).Async(cancellationToken);
}

public sealed class JoinHandler(IZLinkActorManager actors)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, JoinReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        JoinReq message,
        CancellationToken cancellationToken
    )
    {
        if (context.Actors.Bound.Count != 0)
            throw new InvalidOperationException("This session has already joined the lobby.");

        var result = await actors
            .Create(context.SessionId, "participant")
            .InMesh("engine-lobby")
            .Request(new ParticipantActorCreateReq(message.Name))
            .Async(cancellationToken);

        var actor = result switch
        {
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Participant creation was rejected."),
        };

        var bound = await context.Actors.BindAsync(actor, cancellationToken);
        await context
            .Client.Reply(new JoinRes(bound.ActorId, message.Name))
            .Async(cancellationToken);
    }
}
