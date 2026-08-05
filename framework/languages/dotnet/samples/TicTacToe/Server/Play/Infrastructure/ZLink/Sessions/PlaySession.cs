using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
using TicTacToe.Server.Play.Infrastructure.ZLink.Sessions.Handlers;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Sessions;

// --8<-- [start:doc-session]
internal sealed class PlaySession(
    IZLinkSessionContext context,
    ILogger<PlaySession> logger)
    : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<AuthenticatePlaySessionHandler>();
    }

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "client -> play stream: connected. sessionId={SessionId}",
            Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        var boundActors = Context.Actors.Bound.ToArray();
        logger.LogInformation(
            "client -> play stream: disconnected. sessionId={SessionId}, actors={ActorCount}",
            Context.SessionId,
            boundActors.Length);

        foreach (var actor in boundActors) await actor.NotifyDisconnectedAsync(cancellationToken);
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        logger.LogWarning(
            "play stream: error. code={Code}, message={Message}, sessionId={SessionId}",
            error.Error,
            error.Message,
            Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "client -> play stream: message received. name={MessageName}, kind={Kind}, sessionId={SessionId}",
            dispatch.PacketName,
            dispatch.CanReply ? "Request" : "Send",
            Context.SessionId);

        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
            return;

        var actor = RequireSingleBoundActor($"relaying packet '{dispatch.PacketName}'");
        await actor.RelayAsync(payload, cancellationToken);
    }

    private IZLinkSessionActor RequireSingleBoundActor(string action)
    {
        var actors = Context.Actors.Bound;
        return actors.Count switch
        {
            1 => actors.Single(),
            0 => throw new InvalidOperationException($"Client must authenticate before {action}."),
            _ => throw new InvalidOperationException($"Exactly one actor must be bound before {action}.")
        };
    }
}
// --8<-- [end:doc-session]
