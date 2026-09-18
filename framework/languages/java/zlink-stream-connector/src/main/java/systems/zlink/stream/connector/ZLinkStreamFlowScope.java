package systems.zlink.stream.connector;

/**
 * Reads and installs the connector's current flow context.
 *
 * <p>The context itself is a thread local. A runtime whose unit of execution
 * is not a thread - a Kotlin coroutine, for instance - cannot rely on that
 * alone: a suspension point can resume the continuation on another thread,
 * and the outbound call made after it would start a new
 * {@code APPLICATION} flow instead of continuing the inbound one
 * (Java spec 03 7.1). Such a runtime reads the context here while it still
 * holds it and installs it again around each resumption.
 *
 * <p>This is a connector runtime bridge, not an application surface.
 * Application code never needs to pass a flow explicitly.
 */
public final class ZLinkStreamFlowScope implements AutoCloseable {
    private final ZLinkConnectorFlowContext.Scope scope;

    private ZLinkStreamFlowScope(ZLinkConnectorFlowContext.Scope scope) {
        this.scope = scope;
    }

    /**
     * The flow the current thread is running under, or {@code null} when it
     * is not running inside an inbound handler.
     */
    public static ZLinkStreamFlow current() {
        ZLinkConnectorFlowContext.State state = ZLinkConnectorFlowContext.current();
        return state == null ? null : new CapturedFlow(state);
    }

    /**
     * Installs {@code flow} as the current flow until the returned scope is
     * closed, which restores whatever was in effect before. A {@code null}
     * flow installs nothing and closing is then a no-op.
     */
    public static ZLinkStreamFlowScope enter(ZLinkStreamFlow flow) {
        if (flow == null) {
            return new ZLinkStreamFlowScope(null);
        }
        return new ZLinkStreamFlowScope(ZLinkConnectorFlowContext.enter(
            new ZLinkConnectorFlowContext.State(
                flow.flowId(),
                flow.flowOrigin() == null
                    ? ZLinkFlowOrigin.APPLICATION.wireValue()
                    : flow.flowOrigin().wireValue())));
    }

    @Override
    public void close() {
        if (scope != null) {
            scope.close();
        }
    }

    private record CapturedFlow(ZLinkConnectorFlowContext.State state)
        implements ZLinkStreamFlow {
        @Override
        public String flowId() {
            return state.flowId();
        }

        @Override
        public ZLinkFlowOrigin flowOrigin() {
            return ZLinkFlowOrigin.fromWireValue(state.flowOrigin());
        }
    }
}
