package systems.zlink.framework.perf;

import java.util.List;

// The role's own statement that its cell objects (Spot, Actor, subscriptions) are not yet prepared (§16.1 objectsReady).
// The role replaces the statement as its public create/bind results arrive; the evidence lists those results.
public final class ObjectsReadiness {
    private record State(boolean ready, String reason, List<Object> evidence) {}

    private volatile State state;

    public ObjectsReadiness(boolean ready, String reason) {
        state = new State(ready, reason, List.of());
    }

    public boolean ready() {
        return state.ready();
    }

    public String reason() {
        return state.reason();
    }

    public List<Object> evidence() {
        return state.evidence();
    }

    public void set(boolean ready, String reason, List<Object> evidence) {
        state = new State(ready, reason, List.copyOf(evidence));
    }
}
