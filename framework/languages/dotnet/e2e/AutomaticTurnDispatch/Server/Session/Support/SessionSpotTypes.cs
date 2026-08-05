using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Session.Support;

internal sealed class SessionAwaitEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<SessionAwaitActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public ValueTask OnJoinedActorAsync(SessionAwaitActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(SessionAwaitActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class SessionAwaitActorFactory : IZLinkActorFactory<SessionAwaitActor>
{
    public ValueTask<SessionAwaitActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(
            new SessionAwaitActor(context.ActorId, context));
    }
}

internal sealed class SessionAwaitActor(string actorId, IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;
}
