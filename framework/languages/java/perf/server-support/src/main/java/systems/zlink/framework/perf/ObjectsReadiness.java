package systems.zlink.framework.perf;

import java.util.List;

// The role's statement that cell objects are ready (§16.1); evidence may be refreshed without changing that decision.
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

    public synchronized void set(boolean ready, String reason, List<Object> evidence) {
        state = new State(ready, reason, List.copyOf(evidence));
    }

    /** Refresh evidence without changing the role's independently owned readiness decision. */
    synchronized void recordEvidence(List<Object> evidence) {
        State current = state;
        state = new State(current.ready(), current.reason(), List.copyOf(evidence));
    }
}
