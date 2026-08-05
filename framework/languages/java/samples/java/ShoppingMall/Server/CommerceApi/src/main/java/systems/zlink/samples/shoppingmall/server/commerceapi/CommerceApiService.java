package systems.zlink.samples.shoppingmall.server.commerceapi;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.samples.shoppingmall.server.configuration.SampleNames;
import systems.zlink.samples.shoppingmall.server.configuration.SampleTimings;
import systems.zlink.samples.shoppingmall.server.configuration.SampleTopology;
import systems.zlink.samples.shoppingmall.server.shared.domain.OrderDomain;
import systems.zlink.samples.shoppingmall.server.shared.store.RedisCommerceStore;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

public final class CommerceApiService {
    private final RedisCommerceStore store;
    private final ZLinkRouteClient routes;
    private final SampleTopology topology;

    public CommerceApiService(
        RedisCommerceStore store,
        ZLinkRouteClient routes,
        SampleTopology topology) {
        this.store = store;
        this.routes = routes;
        this.topology = topology;
    }

    public CompletionStage<Messages.StartOrderRes> startOrder(Messages.StartOrderReq request) {
        validate(request);
        Messages.CartSeed cart = store.getCart(request.cartId());
        store.validateShippingAddress(request.shippingAddressId());
        store.getPaymentMethod(request.paymentMethodId());
        OrderDomain.IdempotencyMapping mapping = store.reserveIdempotency(
            request.idempotencyKey(),
            topology.api().instanceName());
        store.saveOrderPaymentMethod(mapping.orderId(), request.paymentMethodId());
        Messages.OrderState existing = store.findProjection(mapping.orderId());
        if (existing != null) {
            return CompletableFuture.completedFuture(
                new Messages.StartOrderRes(mapping.orderId(), existing.status()));
        }

        Messages.StartOrderWorkflowReq workflowRequest = workflowRequest(request, mapping.orderId(), cart);
        return workflowRequest(mapping.orderId(), workflowRequest)
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(Messages.StartOrderWorkflowRes.class)
            .thenApply(started -> {
                store.markIdempotencyStarted(request.idempotencyKey());
                return new Messages.StartOrderRes(mapping.orderId(), started.state().status());
            });
    }

    public CompletionStage<Messages.GetOrderStateRes> getOrder(String orderId) {
        return CompletableFuture.completedFuture(
            new Messages.GetOrderStateRes(store.findProjection(orderId)));
    }

    public void deleteProjection(String orderId) {
        store.deleteProjection(orderId);
    }

    public CompletionStage<Messages.RebuildOrderProjectionRes> rebuildProjection(String orderId) {
        return workflowRequest(orderId, new Messages.RebuildOrderProjectionReq(orderId))
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(Messages.RebuildOrderProjectionRes.class);
    }

    public CompletionStage<Messages.StartOrderRes> createPendingThenStart(Messages.StartOrderReq request) {
        store.reserveIdempotency(
            request.idempotencyKey(),
            topology.api().instanceName());
        return startOrder(request);
    }

    public CompletionStage<Messages.ContinueOrderWorkflowRes> prepareInventoryReserved(
        Messages.StartOrderReq request) {
        validate(request);
        OrderDomain.IdempotencyMapping mapping = store.reserveIdempotency(
            request.idempotencyKey(),
            topology.api().instanceName());
        Messages.CartSeed cart = store.getCart(request.cartId());
        store.saveOrderPaymentMethod(mapping.orderId(), request.paymentMethodId());
        Messages.StartOrderWorkflowReq workflowRequest = workflowRequest(request, mapping.orderId(), cart);
        return workflowRequest(
                mapping.orderId(),
                new Messages.PrepareInventoryReservedCheckpointReq(workflowRequest))
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(Messages.ContinueOrderWorkflowRes.class);
    }

    public CompletionStage<Messages.ContinueOrderWorkflowRes> continueOrder(String orderId) {
        return workflowRequest(orderId, new Messages.ContinueOrderWorkflowReq(orderId))
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(Messages.ContinueOrderWorkflowRes.class);
    }

    public Messages.ServerAssertionRes assertEvidence(Messages.ServerAssertionReq request) {
        List<String> orderIds = List.of(
            request.successfulOrderId(),
            request.pendingRecoveredOrderId(),
            request.concurrentOrderId(),
            request.resumedOrderId(),
            request.inventoryFailureOrderId(),
            request.paymentFailureOrderId(),
            request.scaleOutOrderId());
        OrderDomain.StoreEvidence evidence = store.evidence(orderIds);
        List<String> lines = new ArrayList<>();
        boolean passed = true;
        for (String orderId : orderIds) {
            Messages.OrderState state = store.findProjection(orderId);
            String status = state == null ? "missing" : state.status();
            lines.add(orderId + "=" + status + " events=" + evidence.eventsByOrder().get(orderId));
            if (state == null) {
                passed = false;
            }
        }
        passed &= store.findProjection(request.successfulOrderId()) != null
            && Messages.OrderStatuses.Confirmed.equals(store.findProjection(request.successfulOrderId()).status());
        passed &= evidence.paymentFailureCount() >= 1;
        passed &= evidence.releasedReservationCount() >= 1;
        lines.add("paymentFailures=" + evidence.paymentFailureCount());
        lines.add("releasedReservations=" + evidence.releasedReservationCount());
        lines.add("startedIdempotency=" + evidence.startedIdempotencyCount());
        return new Messages.ServerAssertionRes(passed, lines);
    }

    private static Messages.StartOrderWorkflowReq workflowRequest(
        Messages.StartOrderReq request,
        String orderId,
        Messages.CartSeed cart) {
        return new Messages.StartOrderWorkflowReq(
            orderId,
            request.cartId(),
            request.shippingAddressId(),
            request.paymentMethodId(),
            request.idempotencyKey(),
            cart.lines(),
            cart.amount(),
            cart.currency());
    }

    private systems.zlink.framework.spots.ZLinkSpotRequestCall workflowRequest(
        String orderId,
        Object request) {
        return routes.requestToSpot(orderId, request)
            .instanceSpot(SampleNames.OrderWorkflowSpotType)
            .inMesh(SampleNames.OrderSpotDiscovery);
    }

    private static void validate(Messages.StartOrderReq request) {
        requireText(request.cartId(), "cartId");
        requireText(request.shippingAddressId(), "shippingAddressId");
        requireText(request.paymentMethodId(), "paymentMethodId");
        requireText(request.idempotencyKey(), "idempotencyKey");
    }

    private static void requireText(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required.");
        }
    }
}
