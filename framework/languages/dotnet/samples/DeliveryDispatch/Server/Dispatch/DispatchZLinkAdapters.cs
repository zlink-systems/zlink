using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;

namespace DeliveryDispatch.Server.Dispatch;

/// <summary>
/// The only way the worker reaches a courier actor. The offer is a one-way send: the turn that
/// sends it ends right there, and the courier's answer comes back later as its own inbound
/// message (common sample spec §7.4).
/// </summary>
internal sealed class CourierOfferPort(
    Zlink.Framework.Contracts.Actors.IZLinkActorClient actors)
{
    public async ValueTask OfferAsync(
        AssignDeliveryMsg delivery,
        string courierId,
        int attempt,
        CancellationToken cancellationToken)
    {
        await actors
            .SendToActor(
                courierId,
                new OfferDeliveryMsg(
                    courierId,
                    delivery.DeliveryId,
                    attempt,
                    delivery.PickupAddress,
                    delivery.DropoffAddress))
            .Async(cancellationToken);
    }
}

internal sealed class DeliveryStatusPublisher(IZLinkRouteClient channels)
{
    public async ValueTask PublishAsync(
        AssignDeliveryMsg delivery,
        DeliveryStatus status,
        string? courierId,
        CancellationToken cancellationToken)
    {
        _ = await DispatchChannelClient.RequestAsync<DeliveryStatusChangedReq, DeliveryStatusChangedRes>(
            channels,
            SampleNames.TrackingRouteChannel,
            new DeliveryStatusChangedReq(
                delivery.DeliveryId,
                delivery.CustomerId,
                status,
                courierId,
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()),
            cancellationToken);
    }
}

internal static class DispatchChannelClient
{
    public static async ValueTask<TRes> RequestAsync<TReq, TRes>(
        IZLinkRouteClient channels,
        string channelName,
        TReq request,
        CancellationToken cancellationToken,
        TimeSpan? timeout = null)
    {
        var call = channels.RequestToChannel(channelName, request);
        if (timeout is { } value)
        {
            call = call.Timeout(value);
        }

        return await call.Async<TRes>(cancellationToken);
    }
}
