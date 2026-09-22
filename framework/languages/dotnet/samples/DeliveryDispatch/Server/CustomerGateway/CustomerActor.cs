using DeliveryDispatch.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;

namespace DeliveryDispatch.Server.CustomerGateway;

internal sealed class CustomerActor(string actorId, IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    // --8<-- [start:doc-dd-bound-session-push]
    public async ValueTask PushStatusAsync(
        DeliveryStatusUpdatedMsg status,
        CancellationToken cancellationToken
    )
    {
        await Context
            .BoundSession.Send(
                new DeliveryStatusNotify(
                    status.DeliveryId,
                    status.Status,
                    status.CourierId,
                    status.OccurredAtUnixMs
                )
            )
            .Async(cancellationToken);
    }
    // --8<-- [end:doc-dd-bound-session-push]
}

internal sealed class CustomerActorFactory : IZLinkActorFactory<CustomerActor>
{
    public ValueTask<CustomerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    )
    {
        return ValueTask.FromResult(new CustomerActor(context.ActorId, context));
    }
}
