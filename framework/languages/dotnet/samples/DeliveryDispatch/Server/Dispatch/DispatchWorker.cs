using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace DeliveryDispatch.Server.Dispatch;

/// <summary>
/// One delivery's offer in flight. Dispatch never waits for a courier's decision — it writes
/// this row down, and the row decides what happens next (common sample spec §7.4).
/// </summary>
internal sealed record DeliveryOffer(
    AssignDeliveryMsg Request,
    int CandidateIndex,
    int Attempt,
    DateTimeOffset Deadline);

/// <summary>
/// The offers Dispatch is waiting on. This is the whole of the waiting: no blocked thread, no
/// held serial queue, just rows with deadlines.
/// </summary>
internal sealed class DeliveryOfferStore
{
    private readonly object _gate = new();
    private readonly Dictionary<string, MutableOffer> _offers = new(StringComparer.Ordinal);

    /// <summary>Records a new offer and returns its attempt number.</summary>
    public int Offer(AssignDeliveryMsg request, int candidateIndex, TimeSpan timeout)
    {
        lock (_gate)
        {
            if (!_offers.TryGetValue(request.DeliveryId, out var offer))
            {
                offer = new MutableOffer(request);
                _offers[request.DeliveryId] = offer;
            }

            offer.Request = request;
            offer.CandidateIndex = candidateIndex;
            offer.Attempt += 1;
            offer.Deadline = DateTimeOffset.UtcNow + timeout;
            offer.Settled = false;
            return offer.Attempt;
        }
    }

    /// <summary>
    /// Closes the offer a decision belongs to and returns it. A decision naming another attempt
    /// arrived after the offer was reassigned, and an offer already settled has been answered —
    /// both are dropped.
    /// </summary>
    public DeliveryOffer? Settle(string deliveryId, int attempt)
    {
        lock (_gate)
        {
            if (!_offers.TryGetValue(deliveryId, out var offer)
                || offer.Settled
                || offer.Attempt != attempt)
            {
                return null;
            }

            offer.Settled = true;
            return offer.Snapshot();
        }
    }

    /// <summary>The offers whose deadline has passed. The sweeper reassigns them.</summary>
    public IReadOnlyList<DeliveryOffer> TakeExpired()
    {
        var now = DateTimeOffset.UtcNow;
        lock (_gate)
        {
            var expired = new List<DeliveryOffer>();
            foreach (var offer in _offers.Values)
            {
                if (offer.Settled || offer.Deadline > now) continue;

                offer.Settled = true;
                expired.Add(offer.Snapshot());
            }

            return expired;
        }
    }

    public void Close(string deliveryId)
    {
        lock (_gate)
        {
            _offers.Remove(deliveryId);
        }
    }

    private sealed class MutableOffer(AssignDeliveryMsg request)
    {
        public AssignDeliveryMsg Request { get; set; } = request;

        public int CandidateIndex { get; set; }

        public int Attempt { get; set; }

        public DateTimeOffset Deadline { get; set; }

        public bool Settled { get; set; }

        public DeliveryOffer Snapshot() => new(Request, CandidateIndex, Attempt, Deadline);
    }
}

/// <summary>Who gets offered a delivery, and in what order. The worker's policy, not the
/// courier node's.</summary>
internal sealed class CourierSelectionPolicy
{
    public IReadOnlyList<string> Candidates { get; } = ["courier-a", "courier-b"];
}

/// <summary>
/// The dispatch flow. No step of it waits for a courier: the offer goes out one-way, the turn
/// ends, and the offer row decides what happens next — either a decision arrives, or the
/// deadline passes and the sweeper reassigns (common sample spec §7.4).
/// </summary>
internal sealed class DispatchWorker(
    DeliveryOfferStore offers,
    CourierSelectionPolicy couriers,
    CourierOfferPort courierOffers,
    DeliveryStatusPublisher statusPublisher,
    ILogger<DispatchWorker> logger)
{
    /// <summary>The first offer. Records it, sends it, and returns — nobody is left waiting.</summary>
    public async ValueTask StartAsync(AssignDeliveryMsg request, CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "deliverydispatch dispatch: assign delivery={DeliveryId} customer={CustomerId}",
            request.DeliveryId,
            request.CustomerId);

        var courierId = couriers.Candidates[0];
        var attempt = offers.Offer(request, 0, SampleTimings.CourierDecisionTimeout);
        await statusPublisher.PublishAsync(request, DeliveryStatus.Assigned, courierId, cancellationToken);
        await courierOffers.OfferAsync(request, courierId, attempt, cancellationToken);
    }

    /// <summary>A decision arrived. Accepted carries the delivery through; refused reassigns.</summary>
    public async ValueTask SettleAsync(
        DeliveryOffer offer,
        bool accepted,
        string? reason,
        CancellationToken cancellationToken)
    {
        var courierId = couriers.Candidates[offer.CandidateIndex];
        if (accepted)
        {
            await statusPublisher.PublishAsync(offer.Request, DeliveryStatus.Accepted, courierId, cancellationToken);
            // The sample contract includes pickup only on direct acceptance. A reassigned
            // delivery transitions from Accepted directly to Delivered.
            if (offer.CandidateIndex == 0)
            {
                await statusPublisher.PublishAsync(
                    offer.Request,
                    DeliveryStatus.PickedUp,
                    courierId,
                    cancellationToken);
            }
            await statusPublisher.PublishAsync(offer.Request, DeliveryStatus.Delivered, courierId, cancellationToken);
            offers.Close(offer.Request.DeliveryId);
            return;
        }

        logger.LogInformation(
            "deliverydispatch dispatch: courier={CourierId} did not take delivery={DeliveryId} ({Reason})",
            courierId,
            offer.Request.DeliveryId,
            reason ?? "refused");
        await ReassignAsync(offer, cancellationToken);
    }

    /// <summary>
    /// The offer lapsed, or the courier refused. Either way the next candidate gets it. The
    /// deadline lives here rather than on the courier node: a node that timed the offer and
    /// manufactured a refusal would be hiding the dispatch policy (common sample spec §7.4).
    /// </summary>
    public async ValueTask ReassignAsync(DeliveryOffer offer, CancellationToken cancellationToken)
    {
        var nextIndex = offer.CandidateIndex + 1;
        if (nextIndex >= couriers.Candidates.Count)
        {
            await statusPublisher.PublishAsync(
                offer.Request,
                DeliveryStatus.Failed,
                couriers.Candidates[^1],
                cancellationToken);
            offers.Close(offer.Request.DeliveryId);
            logger.LogWarning(
                "deliverydispatch dispatch: delivery={DeliveryId} was rejected by all couriers",
                offer.Request.DeliveryId);
            return;
        }

        var courierId = couriers.Candidates[nextIndex];
        var attempt = offers.Offer(offer.Request, nextIndex, SampleTimings.CourierDecisionTimeout);
        await statusPublisher.PublishAsync(offer.Request, DeliveryStatus.Reassigned, courierId, cancellationToken);
        await courierOffers.OfferAsync(offer.Request, courierId, attempt, cancellationToken);
    }
}

/// <summary>Takes accepted deliveries off the queue and starts them, in order.</summary>
internal sealed class DispatchQueuePump(
    DispatchWorkQueue queue,
    DispatchWorker worker,
    ILogger<DispatchQueuePump> logger)
    : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        await foreach (var request in queue.ReadAllAsync(stoppingToken))
        {
            try
            {
                await worker.StartAsync(request, stoppingToken);
            }
            catch (Exception error) when (!stoppingToken.IsCancellationRequested)
            {
                logger.LogError(
                    error,
                    "deliverydispatch dispatch: could not start delivery={DeliveryId}",
                    request.DeliveryId);
            }
        }
    }
}

/// <summary>
/// The offer deadline. It is a timer, not a wait: nothing is blocked on a courier, so a lapsed
/// offer only moves on because someone comes looking for it (common sample spec §7.4).
/// </summary>
internal sealed class OfferDeadlineSweeper(
    DeliveryOfferStore offers,
    DispatchWorker worker,
    ILogger<OfferDeadlineSweeper> logger)
    : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            await Task.Delay(SampleTimings.OfferSweepInterval, stoppingToken);

            foreach (var offer in offers.TakeExpired())
            {
                logger.LogInformation(
                    "deliverydispatch dispatch: offer expired delivery={DeliveryId} attempt={Attempt}",
                    offer.Request.DeliveryId,
                    offer.Attempt);
                try
                {
                    await worker.ReassignAsync(offer, stoppingToken);
                }
                catch (Exception error) when (!stoppingToken.IsCancellationRequested)
                {
                    // The sweeper comes back next tick; one failed reassign must not stop it.
                    logger.LogError(
                        error,
                        "deliverydispatch dispatch: reassign failed delivery={DeliveryId}",
                        offer.Request.DeliveryId);
                }
            }
        }
    }
}
