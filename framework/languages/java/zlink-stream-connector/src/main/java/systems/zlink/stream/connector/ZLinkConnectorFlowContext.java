package systems.zlink.stream.connector;

final class ZLinkConnectorFlowContext {
    private static final ThreadLocal<State> CURRENT = new ThreadLocal<>();

    private ZLinkConnectorFlowContext() { }

    static State currentOrApplication() {
        State current = CURRENT.get();
        return current == null ? new State(ZLinkConnectorFlowIds.next(), 3) : current;
    }

    static State inbound(String flowId, int flowOrigin) {
        return flowId == null
            ? new State(ZLinkConnectorFlowIds.next(), 1)
            : new State(flowId, flowOrigin);
    }

    static Scope enter(State state) {
        State previous = CURRENT.get();
        CURRENT.set(state);
        return () -> {
            if (previous == null) {
                CURRENT.remove();
            } else {
                CURRENT.set(previous);
            }
        };
    }

    record State(String flowId, int flowOrigin) { }

    @FunctionalInterface
    interface Scope extends AutoCloseable {
        @Override
        void close();
    }
}
