using DeliveryDispatch.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;

namespace DeliveryDispatch.Server.CourierActorNode;

/// <summary>
/// A courier. It pushes the offer to whoever holds this courier's stream session and its turn
/// ends there — it does not wait for the answer, and there is nothing left here that could wait
/// (common sample spec §7.4). The answer arrives later as a <see cref="CourierDecisionMsg"/>
/// from the session, and the only thing the actor has to remember in between is which attempt
/// the offer belonged to, so the decision can be paired with it.
/// </summary>
internal sealed class CourierActor(
    string actorId,
    IZLinkActorContext context) : IZLinkActor
{
    private readonly Dictionary<string, int> _offeredAttempts = new(StringComparer.Ordinal);

    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    /// <summary>Pushes the offer and returns. The courier takes as long as it takes.</summary>
    public async ValueTask OfferAsync(OfferDeliveryMsg offer, CancellationToken cancellationToken)
    {
        _offeredAttempts[offer.DeliveryId] = offer.Attempt;
        await Context.BoundSession
            .Send(new OfferDeliveryNotify(
                offer.CourierId,
                offer.DeliveryId,
                offer.PickupAddress,
                offer.DropoffAddress))
            .Async(cancellationToken);
    }

    /// <summary>
    /// The attempt the courier is answering, or null when this actor knows of no such offer —
    /// it was already answered, or this actor was never offered it.
    /// </summary>
    public int? TakeOfferedAttempt(string deliveryId)
    {
        return _offeredAttempts.Remove(deliveryId, out var attempt) ? attempt : null;
    }
}

internal sealed class CourierActorFactory : IZLinkActorFactory<CourierActor>
{
    public ValueTask<CourierActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        return ValueTask.FromResult(new CourierActor(context.ActorId, context));
    }
}
