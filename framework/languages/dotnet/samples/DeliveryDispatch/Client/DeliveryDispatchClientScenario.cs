using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace DeliveryDispatch.Client;

internal sealed class DeliveryDispatchClientScenario(ILogger logger)
{
    // End-to-end client story:
    // 1. Open one customer stream session and two courier stream sessions.
    // 2. Bind courier-a and courier-b independently so each courier actor can push to its own client.
    // 3. Create a delivery where courier-a receives an offer and accepts it.
    // 4. Create another delivery where courier-a receives an offer but stays silent.
    // 5. Let the dispatch server time out courier-a, reassign to courier-b, and finish the delivery.
    // 6. Verify both ordered status sequences from the customer-facing notifications.
    public async ValueTask RunAsync(
        ZLinkHttpClient http,
        IZlinkStreamConnector customer,
        IZlinkStreamConnector courierA,
        IZlinkStreamConnector courierB,
        CancellationToken cancellationToken = default)
    {
        // The sample uses three logical client sessions:
        // one customer session and one independent courier session per courier.
        await customer.Connect.Async(cancellationToken);
        await courierA.Connect.Async(cancellationToken);
        await courierB.Connect.Async(cancellationToken);

        // Each courier binds its own stream session to the actor chosen by the server side directory.
        var courierABinding = await courierA.Request(new BindCourierSessionReq("courier-a"))
            .Async<BindCourierSessionRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(courierABinding.CourierId == "courier-a", "courier-a binding id mismatch.");
        var courierBBinding = await courierB.Request(new BindCourierSessionReq("courier-b"))
            .Async<BindCourierSessionRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(courierBBinding.CourierId == "courier-b", "courier-b binding id mismatch.");
        logger.LogInformation("topology=ready");

        // Run both dispatch paths: direct acceptance first, then timeout-based reassignment.
        await RunSuccessfulDeliveryAsync(http, customer, courierA, courierB, cancellationToken);
        await RunReassignedDeliveryAsync(http, customer, courierA, courierB, cancellationToken);
    }

    private static async ValueTask RunSuccessfulDeliveryAsync(
        ZLinkHttpClient http,
        IZlinkStreamConnector customer,
        IZlinkStreamConnector courierA,
        IZlinkStreamConnector courierB,
        CancellationToken cancellationToken)
    {
        // Success path: customer subscribes, dispatch creates the delivery,
        // courier-a receives the offer through stream push, then accepts.
        var deliveryId = "delivery-success";

        // Register waits before creating the delivery so push messages cannot race past the client.
        var offer = courierA.WaitFor<OfferDeliveryNotify>()
            .Where(message => message.Payload.DeliveryId == deliveryId)
            .Async(cancellationToken);
        var noCourierBOffer = courierB.ExpectNone<OfferDeliveryNotify>()
            .Within(TimeSpan.FromSeconds(1))
            .Async(cancellationToken).AsTask();

        var subscribed = await customer.Request(new SubscribeDeliveryReq(deliveryId))
            .Async<SubscribeDeliveryRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(subscribed.DeliveryId == deliveryId, "success subscription id mismatch.");
        var statusSequenceTask = customer.WaitForSequence<DeliveryStatusNotify>()
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Assigned }
                               && id == deliveryId)
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Accepted }
                               && id == deliveryId)
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.PickedUp }
                               && id == deliveryId)
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Delivered }
                               && id == deliveryId)
            .Timeout(customer.Options.WaitTimeout)
            .Async(cancellationToken).AsTask();

        var created = await http.Post("/deliveries")
            .Body(new CreateDeliveryReq(
                deliveryId,
                "customer-1",
                "Kitchen 12",
                "Customer Lobby"))
            .Fetch<CreateDeliveryRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(created.DeliveryId == deliveryId, "created success delivery id mismatch.");

        // courier-a receives the offer through its bound stream session and accepts it.
        var courierOffer = (await offer).Payload;
        await courierA.Send(new CourierDecisionMsg(
                courierOffer.DeliveryId,
                courierOffer.CourierId,
                true,
                null))
            .Async(cancellationToken);

        var statuses = await statusSequenceTask;
        var assigned = statuses[0].Payload;
        var accepted = statuses[1].Payload;
        var pickedUp = statuses[2].Payload;
        var delivered = statuses[3].Payload;

        ZlinkStreamAssert.Ensure(assigned.CourierId == "courier-a", "assigned courier mismatch.");
        ZlinkStreamAssert.Ensure(accepted.CourierId == "courier-a", "accepted courier mismatch.");
        ZlinkStreamAssert.Ensure(pickedUp.CourierId == "courier-a", "pickup courier mismatch.");
        ZlinkStreamAssert.Ensure(delivered.CourierId == "courier-a", "delivered courier mismatch.");
        await noCourierBOffer;
    }

    private async ValueTask RunReassignedDeliveryAsync(
        ZLinkHttpClient http,
        IZlinkStreamConnector customer,
        IZlinkStreamConnector courierA,
        IZlinkStreamConnector courierB,
        CancellationToken cancellationToken)
    {
        // Reassignment path: courier-a receives the first offer and does not respond.
        // The server timeout causes the same delivery to be offered to courier-b.
        var deliveryId = "delivery-reassign";

        // courier-a and courier-b are different stream sessions, so each waits on its own connector.
        var firstOffer = courierA.WaitFor<OfferDeliveryNotify>()
            .Where(message => message.Payload.DeliveryId == deliveryId)
            .Where(message => message.Payload.CourierId == "courier-a")
            .Async(cancellationToken);
        var secondOffer = courierB.WaitFor<OfferDeliveryNotify>()
            .Where(message => message.Payload.DeliveryId == deliveryId)
            .Where(message => message.Payload.CourierId == "courier-b")
            .Async(cancellationToken);

        var subscribed = await customer.Request(new SubscribeDeliveryReq(deliveryId))
            .Async<SubscribeDeliveryRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(subscribed.DeliveryId == deliveryId, "reassignment subscription id mismatch.");
        var statusSequenceTask = customer.WaitForSequence<DeliveryStatusNotify>()
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Assigned }
                               && id == deliveryId)
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Reassigned }
                               && id == deliveryId)
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Accepted }
                               && id == deliveryId)
            .Expect(message => message.Payload is { DeliveryId: var id, Status: DeliveryStatus.Delivered }
                               && id == deliveryId)
            .Timeout(customer.Options.WaitTimeout)
            .Async(cancellationToken).AsTask();

        var created = await http.Post("/deliveries")
            .Body(new CreateDeliveryReq(
                deliveryId,
                "customer-1",
                "Kitchen 12",
                "Customer Lobby"))
            .Fetch<CreateDeliveryRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(created.DeliveryId == deliveryId, "created reassignment delivery id mismatch.");

        // courier-a intentionally does not answer. The dispatch server times out and offers the
        // same delivery to courier-b, which accepts through its own bound stream session.
        _ = await firstOffer;
        var acceptedOffer = (await secondOffer).Payload;
        await courierB.Send(new CourierDecisionMsg(
                acceptedOffer.DeliveryId,
                acceptedOffer.CourierId,
                true,
                null))
            .Async(cancellationToken);

        var statuses = await statusSequenceTask;
        var assigned = statuses[0].Payload;
        var reassigned = statuses[1].Payload;
        var accepted = statuses[2].Payload;
        var delivered = statuses[3].Payload;

        ZlinkStreamAssert.Ensure(assigned.CourierId == "courier-a", "initial courier mismatch.");
        ZlinkStreamAssert.Ensure(reassigned.CourierId == "courier-b", "reassigned courier mismatch.");
        ZlinkStreamAssert.Ensure(accepted.CourierId == "courier-b", "accepting courier mismatch.");
        ZlinkStreamAssert.Ensure(delivered.CourierId == "courier-b", "final courier mismatch.");
        logger.LogInformation("deliverydispatch-reassignment=completed");
    }
}
