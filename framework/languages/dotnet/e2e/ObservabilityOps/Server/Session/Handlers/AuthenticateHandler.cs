using ObservabilityOps.Server.Support;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Streams;

namespace ObservabilityOps.Server.Session.Handlers;

internal sealed class AuthenticateHandler(IZLinkActorManager actors)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, AuthenticateReq>
{
    public async ValueTask HandleAsync(IZLinkSessionContext context, ZLinkSessionDispatchContext dispatch,
        AuthenticateReq request, CancellationToken cancellationToken)
    {
        _ = dispatch;
        var created = await actors
            .GetOrCreate(request.ActorId, ObservabilityNames.PlayerActorType)
            .InMesh(ObservabilityNames.PlayMesh)
            .Request(new EnsurePlayerReq(request.ActorId))
            .Async(cancellationToken);
        var actor = created switch
        {
            ZLinkActorCreateResult.Existing existing => existing.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            ZLinkActorCreateResult.Rejected =>
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' creation was rejected."),
            _ => throw new InvalidOperationException("Unknown Actor creation result.")
        };
        await context.Actors.BindOrGetAsync(actor, cancellationToken);
        await context.Client.Reply(new AuthenticateRes(
                actor.ActorId,
                actor.NodeRid.ToString(),
                actor.ObjectGeneration))
            .Async(cancellationToken);
    }
}

internal sealed class SessionBoundedOperationHandler(BoundedOperationGate gate)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, SessionBoundedOperationReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        SessionBoundedOperationReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        await gate.EnterAsync(cancellationToken);
        await context.Client.Reply(new SessionBoundedOperationRes(request.Marker))
            .Async(cancellationToken);
    }
}
