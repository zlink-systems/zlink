package systems.zlink.samples.deliverydispatch.server.courierspotnode;

import java.util.HashMap;
import java.util.Map;

public final class ActorDirectory {
    private final Object gate = new Object();
    private final Map<String, CourierActor> actors = new HashMap<>();

    public void register(CourierActor actor) {
        synchronized (gate) {
            actors.put(actor.actorId(), actor);
        }
    }

    public CourierActor require(String actorId) {
        synchronized (gate) {
            CourierActor actor = actors.get(actorId);
            if (actor != null) {
                return actor;
            }
        }
        throw new IllegalStateException("Courier actor is not registered: " + actorId);
    }

    public void remove(String actorId) {
        synchronized (gate) {
            actors.remove(actorId);
        }
    }

}
