package systems.zlink.samples.deliverydispatch.server.customergateway;

import java.util.HashMap;
import java.util.Map;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

public final class CustomerActorDirectory {
    private final Object gate = new Object();
    private final Map<String, CustomerActor> actors = new HashMap<>();
    private final Map<String, String> deliveryCustomers = new HashMap<>();

    public void register(CustomerActor actor) {
        synchronized (gate) {
            actors.put(actor.actorId(), actor);
        }
    }

    public void remove(String actorId) {
        synchronized (gate) {
            actors.remove(actorId);
            deliveryCustomers.values().removeIf(actorId::equals);
        }
    }

    public void subscribe(String customerId, String deliveryId) {
        synchronized (gate) {
            deliveryCustomers.put(deliveryId, customerId);
        }
    }

    public void push(Messages.DeliveryStatusChangedReq status) {
        CustomerActor actor;
        synchronized (gate) {
            String customerId = deliveryCustomers.get(status.deliveryId());
            actor = customerId == null ? null : actors.get(customerId);
        }
        if (actor == null) {
            return;
        }
        actor.push(new Messages.DeliveryStatusNotify(
            status.deliveryId(),
            status.status(),
            status.courierId(),
            status.occurredAt()));
    }
}
