package systems.zlink.samples.deliverydispatch.server.configuration;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

public final class EvidenceStore {
    private final Map<String, List<Messages.DeliveryStatusNotify>> deliveries = new ConcurrentHashMap<>();

    public void append(Messages.DeliveryStatusChangedReq change) {
        deliveries.computeIfAbsent(change.deliveryId(), ignored -> new ArrayList<>())
            .add(new Messages.DeliveryStatusNotify(
                change.deliveryId(),
                change.status(),
                change.courierId(),
                change.occurredAt()));
    }

    public List<Messages.DeliveryStatusNotify> statuses(String deliveryId) {
        return List.copyOf(deliveries.getOrDefault(deliveryId, List.of()));
    }

    public Messages.ServerAssertionResponse assertSequences(String successfulDeliveryId, String reassignedDeliveryId) {
        List<String> evidence = new ArrayList<>();
        boolean successful = matches(
            statuses(successfulDeliveryId),
            List.of(
                expected(Messages.DeliveryStatus.Assigned, "courier-a"),
                expected(Messages.DeliveryStatus.Accepted, "courier-a"),
                expected(Messages.DeliveryStatus.PickedUp, "courier-a"),
                expected(Messages.DeliveryStatus.Delivered, "courier-a")),
            evidence,
            successfulDeliveryId);
        boolean reassigned = matches(
            statuses(reassignedDeliveryId),
            List.of(
                expected(Messages.DeliveryStatus.Assigned, "courier-a"),
                expected(Messages.DeliveryStatus.Reassigned, "courier-b"),
                expected(Messages.DeliveryStatus.Accepted, "courier-b"),
                expected(Messages.DeliveryStatus.PickedUp, "courier-b"),
                expected(Messages.DeliveryStatus.Delivered, "courier-b")),
            evidence,
            reassignedDeliveryId);
        return new Messages.ServerAssertionResponse(successful && reassigned, evidence.toArray(String[]::new));
    }

    private static ExpectedStatus expected(Messages.DeliveryStatus status, String courierId) {
        return new ExpectedStatus(status, courierId);
    }

    private static boolean matches(
        List<Messages.DeliveryStatusNotify> actual,
        List<ExpectedStatus> expected,
        List<String> evidence,
        String deliveryId) {
        if (actual.size() != expected.size()) {
            evidence.add(deliveryId + " expected " + expected.size() + " statuses but saw " + actual.size());
            return false;
        }
        for (int index = 0; index < expected.size(); index++) {
            Messages.DeliveryStatusNotify actualStatus = actual.get(index);
            ExpectedStatus expectedStatus = expected.get(index);
            if (actualStatus.status() != expectedStatus.status()
                || !expectedStatus.courierId().equals(actualStatus.courierId())) {
                evidence.add(deliveryId + " status[" + index + "] expected "
                    + expectedStatus.status() + "/" + expectedStatus.courierId()
                    + " but saw " + actualStatus.status() + "/" + actualStatus.courierId());
                return false;
            }
        }
        evidence.add(deliveryId + " status sequence verified");
        return true;
    }

    private record ExpectedStatus(Messages.DeliveryStatus status, String courierId) {
    }
}
