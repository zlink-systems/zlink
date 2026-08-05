using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace DeliveryDispatch.Server.CourierActorNode.Spots.EntrySpot;

internal sealed class CourierEntrySpot(
    IZLinkEntrySpotContext context,
    ActorDirectory actors,
    ILogger<CourierEntrySpot> logger) : IZLinkEntrySpot<CourierActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        CourierActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        actors.Register(actor);
        logger.LogInformation(
            "deliverydispatch courier-entry: actor created courier={CourierId}",
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

    public ValueTask OnJoinedActorAsync(CourierActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(CourierActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}
