package systems.zlink.samples.shoppingmall.server.shared.domain;

import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

public final class OrderProjection {
    private OrderProjection() {
    }

    public static Messages.OrderState fold(
        Messages.OrderState current,
        OrderDomain.OrderEvent event) {
        return switch (event) {
            case OrderDomain.OrderStartedEvent started -> new Messages.OrderState(
                started.orderId(),
                Messages.OrderStatuses.Created,
                started.shippingAddressId(),
                null,
                null,
                null,
                started.amount(),
                started.currency(),
                started.createdAtUnixMs());
            case OrderDomain.InventoryReservedEvent reserved -> copy(
                require(current, event),
                Messages.OrderStatuses.InventoryReserved,
                reserved.reservationId(),
                null,
                null,
                reserved.createdAtUnixMs());
            case OrderDomain.InventoryReservationFailedEvent failed -> copy(
                require(current, event),
                Messages.OrderStatuses.Failed,
                null,
                null,
                failed.reason(),
                failed.createdAtUnixMs());
            case OrderDomain.PaymentAuthorizedEvent paid -> copy(
                require(current, event),
                Messages.OrderStatuses.PaymentAuthorized,
                null,
                paid.paymentId(),
                null,
                paid.createdAtUnixMs());
            case OrderDomain.PaymentFailedEvent failed -> copy(
                require(current, event),
                Messages.OrderStatuses.Failed,
                null,
                null,
                failed.reason(),
                failed.createdAtUnixMs());
            case OrderDomain.InventoryReleasedEvent released -> copy(
                require(current, event),
                null,
                null,
                null,
                released.reason(),
                released.createdAtUnixMs());
            case OrderDomain.OrderConfirmedEvent confirmed -> copy(
                require(current, event),
                Messages.OrderStatuses.Confirmed,
                null,
                null,
                null,
                confirmed.createdAtUnixMs());
            case OrderDomain.OrderFailedEvent failed -> copy(
                require(current, event),
                Messages.OrderStatuses.Failed,
                null,
                null,
                failed.reason(),
                failed.createdAtUnixMs());
        };
    }

    public static Messages.OrderState fold(java.util.List<OrderDomain.StoredOrderEvent> events) {
        Messages.OrderState state = null;
        for (OrderDomain.StoredOrderEvent event : events) {
            state = fold(state, event.event());
        }
        return state;
    }

    private static Messages.OrderState require(
        Messages.OrderState current,
        OrderDomain.OrderEvent event) {
        if (current == null) {
            throw new IllegalStateException(
                "Projection cannot apply '" + event.eventType() + "' before OrderStartedEvent.");
        }
        return current;
    }

    private static Messages.OrderState copy(
        Messages.OrderState current,
        String status,
        String reservationId,
        String paymentId,
        String reason,
        long updatedAtUnixMs) {
        return new Messages.OrderState(
            current.orderId(),
            status == null ? current.status() : status,
            current.shippingAddressId(),
            reservationId == null ? current.reservationId() : reservationId,
            paymentId == null ? current.paymentId() : paymentId,
            reason == null ? current.reason() : reason,
            current.amount(),
            current.currency(),
            updatedAtUnixMs);
    }
}
