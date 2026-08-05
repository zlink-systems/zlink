using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace DeliveryDispatch.Server.CustomerGateway.Spots.EntrySpot;

internal sealed class CustomerEntrySpot(
    IZLinkEntrySpotContext context,
    CustomerActorDirectory actors,
    ILogger<CustomerEntrySpot> logger) : IZLinkEntrySpot<CustomerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public async ValueTask PushStatusAsync(
        DeliveryStatusUpdatedMsg status,
        CancellationToken cancellationToken)
    {
        await actors.PushAsync(status, cancellationToken);
    }

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        CustomerActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        actors.Register(actor);
        logger.LogInformation(
            "deliverydispatch customer-entry: actor created customer={ActorId}",
            actor.ActorId);
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(CustomerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(CustomerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}
