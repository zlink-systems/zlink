package systems.zlink.samples.shoppingmall.server.shared.domain;
import java.util.Map;

import java.math.BigDecimal;
import java.util.List;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

public final class OrderDomain {
    private OrderDomain() {
    }

    public sealed interface OrderEvent permits
        OrderStartedEvent,
        InventoryReservedEvent,
        InventoryReservationFailedEvent,
        PaymentAuthorizedEvent,
        PaymentFailedEvent,
        InventoryReleasedEvent,
        OrderConfirmedEvent,
        OrderFailedEvent {
        String eventId();

        String orderId();

        long createdAtUnixMs();

        default String eventType() {
            return getClass().getSimpleName();
        }
    }

    public record OrderStartedEvent(
        String eventId,
        String sourceCommandId,
        String orderId,
        String cartId,
        String shippingAddressId,
        List<Messages.OrderLineInput> lines,
        BigDecimal amount,
        String currency,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record InventoryReservedEvent(
        String eventId,
        String orderId,
        String reservationId,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record InventoryReservationFailedEvent(
        String eventId,
        String orderId,
        String reason,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record PaymentAuthorizedEvent(
        String eventId,
        String orderId,
        String paymentId,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record PaymentFailedEvent(
        String eventId,
        String orderId,
        String reason,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record InventoryReleasedEvent(
        String eventId,
        String orderId,
        String reservationId,
        String reason,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record OrderConfirmedEvent(
        String eventId,
        String orderId,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record OrderFailedEvent(
        String eventId,
        String orderId,
        String reason,
        long createdAtUnixMs) implements OrderEvent {
    }

    public record StoredOrderEvent(
        String eventId,
        String sourceCommandId,
        String orderId,
        String eventType,
        OrderEvent event,
        long version,
        long createdAtUnixMs) {
    }

    public record IdempotencyMapping(
        String idempotencyKey,
        String orderId,
        String ownerInstanceId,
        boolean started) {
    }

    public record InventoryReservationLine(String sku, int quantity) {
    }

    public record InventoryReservation(
        String orderId,
        List<InventoryReservationLine> lines) {
    }

    public record ReserveInventoryResult(
        boolean accepted,
        String reservationId,
        String reason) {
    }

    public record AuthorizePaymentResult(
        boolean accepted,
        String paymentId,
        String reason) {
    }

    public record StoreEvidence(
        Map<String, List<String>> eventsByOrder,
        int paymentFailureCount,
        int releasedReservationCount,
        int startedIdempotencyCount) {
    }
}
