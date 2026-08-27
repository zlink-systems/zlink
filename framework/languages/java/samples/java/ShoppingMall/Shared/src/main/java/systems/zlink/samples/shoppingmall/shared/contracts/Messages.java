package systems.zlink.samples.shoppingmall.shared.contracts;

import java.math.BigDecimal;
import java.util.List;

public final class Messages {
    private Messages() {
    }

    public static final class OrderStatuses {
        public static final String Created = "Created";
        public static final String InventoryReserved = "InventoryReserved";
        public static final String PaymentAuthorized = "PaymentAuthorized";
        public static final String Confirmed = "Confirmed";
        public static final String Failed = "Failed";

        private OrderStatuses() {
        }
    }

    public record StartOrderReq(
        String cartId,
        String shippingAddressId,
        String paymentMethodId,
        String idempotencyKey) {
    }

    public record StartOrderRes(String orderId, String status) {
    }

    public record GetOrderStateReq(String orderId) {
    }

    public record GetOrderStateRes(OrderState state) {
    }

    public record OrderState(
        String orderId,
        String status,
        String shippingAddressId,
        String reservationId,
        String paymentId,
        String reason,
        BigDecimal amount,
        String currency,
        long updatedAtUnixMs) {
    }

    public record OrderLineInput(String sku, int quantity) {
    }

    public record CartSeed(
        String cartId,
        List<OrderLineInput> lines,
        BigDecimal amount,
        String currency) {
    }

    public record InventorySeed(String sku, int availableQuantity) {
    }

    public record PaymentMethodSeed(
        String paymentMethodId,
        boolean shouldAuthorize,
        String failureReason) {
    }

    public record StartOrderWorkflowReq(
        String orderId,
        String cartId,
        String shippingAddressId,
        String paymentMethodId,
        String idempotencyKey,
        List<OrderLineInput> lines,
        BigDecimal amount,
        String currency) {
    }

    public record StartOrderWorkflowRes(OrderState state) {
    }

    public record ContinueOrderWorkflowReq(String orderId) {
    }

    public record RunOrderWorkflowMsg(String orderId) {
    }

    public record ContinueOrderWorkflowRes(OrderState state, long objectGeneration) {
    }

    public record RebuildOrderProjectionReq(String orderId) {
    }

    public record RebuildOrderProjectionRes(OrderState state) {
    }

    public record PrepareInventoryReservedCheckpointReq(StartOrderWorkflowReq request) {
    }

    public record ServerAssertionReq(
        String successfulOrderId,
        String pendingRecoveredOrderId,
        String concurrentOrderId,
        String resumedOrderId,
        String inventoryFailureOrderId,
        String paymentFailureOrderId,
        String scaleOutOrderId) {
    }

    public record ServerAssertionRes(boolean passed, List<String> evidence) {
    }
}
