package systems.zlink.samples.deliverydispatch.client;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.stream.connector.ZLinkStreamAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class DeliveryDispatchClientScenario {
    private final String dispatchHttpEndpoint;

    public DeliveryDispatchClientScenario(String dispatchHttpEndpoint) {
        this.dispatchHttpEndpoint = dispatchHttpEndpoint;
    }

    public CompletionStage<Void> run(
        ZLinkStreamConnector customer,
        ZLinkStreamConnector courierA,
        ZLinkStreamConnector courierB) {
        return CompletableFuture.allOf(
                customer.connect().submit().toCompletableFuture(),
                courierA.connect().submit().toCompletableFuture(),
                courierB.connect().submit().toCompletableFuture())
            .thenCompose(ignored -> courierA.request(new Messages.BindCourierSessionReq("courier-a"))
                .submit(Messages.BindCourierSessionRes.class))
            .thenCompose(courierABound -> {
                ZLinkStreamAssert.ensure(
                    courierABound.courierId().equals("courier-a"),
                    "courier-a binding id mismatch");
                return courierB.request(new Messages.BindCourierSessionReq("courier-b"))
                    .submit(Messages.BindCourierSessionRes.class)
                    .thenApply(courierBBound -> {
                        ZLinkStreamAssert.ensure(
                            courierBBound.courierId().equals("courier-b"),
                            "courier-b binding id mismatch");
                        return null;
                    });
            })
            .thenCompose(ignored -> runSuccessfulDelivery(customer, courierA, courierB))
            .thenCompose(ignored -> runReassignedDelivery(customer, courierA, courierB))
            .thenCompose(ignored -> assertServerEvidence());
    }

    private CompletionStage<Void> runSuccessfulDelivery(
        ZLinkStreamConnector customer,
        ZLinkStreamConnector courier,
        ZLinkStreamConnector otherCourier) {
        String deliveryId = "delivery-success";
        CompletionStage<ZLinkStreamMessage<Messages.OfferDeliveryNotify>> offer =
            courier.waitFor(Messages.OfferDeliveryNotify.class)
                .where(Messages.OfferDeliveryNotify.class, message -> message.payload().deliveryId().equals(deliveryId))
                .submit(Messages.OfferDeliveryNotify.class);
        CompletionStage<Void> noOtherCourierOffer = otherCourier
            .expectNone(Messages.OfferDeliveryNotify.class)
            .within(Duration.ofSeconds(1))
            .submit();
        CompletionStage<List<ZLinkStreamMessage<Messages.DeliveryStatusNotify>>> statuses =
            customer.waitForSequence(Messages.DeliveryStatusNotify.class)
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Assigned))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Accepted))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.PickedUp))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Delivered))
                .submit(Messages.DeliveryStatusNotify.class);

        return customer.request(new Messages.SubscribeDeliveryReq(deliveryId))
            .submit(Messages.SubscribeDeliveryRes.class)
            .thenCompose(subscribed -> {
                ZLinkStreamAssert.ensure(
                    subscribed.deliveryId().equals(deliveryId),
                    "success subscription id mismatch");
                return post("/deliveries", new Messages.CreateDeliveryReq(
                    deliveryId, "customer-1", "Kitchen 12", "Customer Lobby"),
                    Messages.CreateDeliveryRes.class);
            })
            .thenCompose(created -> {
                ZLinkStreamAssert.ensure(
                    created.deliveryId().equals(deliveryId),
                    "created success delivery id mismatch");
                return offer;
            })
            .thenCompose(message -> {
                Messages.OfferDeliveryNotify courierOffer = message.payload();
                return courier.send(new Messages.CourierDecision(
                    courierOffer.deliveryId(), courierOffer.courierId(), true, null)).submit();
            })
            .thenCompose(ignored -> statuses)
            .thenCompose(notifications -> {
                ZLinkStreamAssert.ensure(
                    notifications.stream().allMatch(message ->
                        message.payload().courierId().equals("courier-a")),
                    "success delivery status courier mismatch");
                return noOtherCourierOffer;
            });
    }

    private CompletionStage<Void> runReassignedDelivery(
        ZLinkStreamConnector customer,
        ZLinkStreamConnector courierA,
        ZLinkStreamConnector courierB) {
        String deliveryId = "delivery-reassign";
        CompletionStage<ZLinkStreamMessage<Messages.OfferDeliveryNotify>> firstOffer =
            courierA.waitFor(Messages.OfferDeliveryNotify.class)
                .where(Messages.OfferDeliveryNotify.class, message ->
                    message.payload().deliveryId().equals(deliveryId)
                        && message.payload().courierId().equals("courier-a"))
                .submit(Messages.OfferDeliveryNotify.class);
        CompletionStage<ZLinkStreamMessage<Messages.OfferDeliveryNotify>> secondOffer =
            courierB.waitFor(Messages.OfferDeliveryNotify.class)
                .where(Messages.OfferDeliveryNotify.class, message ->
                    message.payload().deliveryId().equals(deliveryId)
                        && message.payload().courierId().equals("courier-b"))
                .submit(Messages.OfferDeliveryNotify.class);
        CompletionStage<List<ZLinkStreamMessage<Messages.DeliveryStatusNotify>>> statuses =
            customer.waitForSequence(Messages.DeliveryStatusNotify.class)
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Assigned))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Reassigned))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Accepted))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.PickedUp))
                .expect(Messages.DeliveryStatusNotify.class, message ->
                    matchesStatus(message, deliveryId, Messages.DeliveryStatus.Delivered))
                .submit(Messages.DeliveryStatusNotify.class);

        return customer.request(new Messages.SubscribeDeliveryReq(deliveryId))
            .submit(Messages.SubscribeDeliveryRes.class)
            .thenCompose(subscribed -> {
                ZLinkStreamAssert.ensure(
                    subscribed.deliveryId().equals(deliveryId),
                    "reassignment subscription id mismatch");
                return post("/deliveries", new Messages.CreateDeliveryReq(
                    deliveryId, "customer-1", "Kitchen 12", "Customer Lobby"),
                    Messages.CreateDeliveryRes.class);
            })
            .thenCompose(created -> {
                ZLinkStreamAssert.ensure(
                    created.deliveryId().equals(deliveryId),
                    "created reassignment delivery id mismatch");
                return firstOffer;
            })
            .thenCompose(ignored -> secondOffer)
            .thenCompose(message -> {
                Messages.OfferDeliveryNotify acceptedOffer = message.payload();
                return courierB.send(new Messages.CourierDecision(
                    acceptedOffer.deliveryId(), acceptedOffer.courierId(), true, null)).submit();
            })
            .thenCompose(ignored -> statuses)
            .thenAccept(notifications -> {
                ZLinkStreamAssert.ensure(
                    notifications.get(0).payload().courierId().equals("courier-a"),
                    "initial courier mismatch");
                ZLinkStreamAssert.ensure(
                    notifications.subList(1, notifications.size()).stream()
                        .allMatch(message -> message.payload().courierId().equals("courier-b")),
                    "reassigned courier mismatch");
                System.out.println(SampleNames.ReassignmentMarker);
            });
    }

    private static boolean matchesStatus(
        ZLinkStreamMessage<Messages.DeliveryStatusNotify> message,
        String deliveryId,
        Messages.DeliveryStatus status) {
        return message.payload().deliveryId().equals(deliveryId)
            && message.payload().status() == status;
    }

    private CompletionStage<Void> assertServerEvidence() {
        return post(
            "/self-check/assert",
            new Messages.ServerAssertionRequest("delivery-success", "delivery-reassign"),
            Messages.ServerAssertionResponse.class).thenAccept(response -> {
                ZLinkStreamAssert.ensure(response.passed(), "server delivery evidence failed");
                System.out.println(SampleNames.ServerEvidenceMarker);
            });
    }

    private <TResponse> CompletionStage<TResponse> post(
        String path,
        Object body,
        Class<TResponse> responseType) {
        return ZLinkHttpClient.create(dispatchHttpEndpoint)
            .post(path)
            .body(body)
            .fetch(responseType);
    }
}
