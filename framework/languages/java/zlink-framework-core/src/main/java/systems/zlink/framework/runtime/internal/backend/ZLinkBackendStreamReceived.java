package systems.zlink.framework.runtime.internal.backend;

import java.util.Objects;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

/**
 * One complete PACKET-mode STREAM result owned by the framework receive loop.
 */
public final class ZLinkBackendStreamReceived implements AutoCloseable {
    private final Optional<RoutingId> routingId;
    private final Message header;
    private final Message body;
    private final Runnable closeAction;
    private boolean closed;

    public ZLinkBackendStreamReceived(
        Optional<RoutingId> routingId,
        Message header,
        Message body,
        Runnable closeAction) {
        this.routingId = routingId == null ? Optional.empty() : routingId;
        this.header = Objects.requireNonNull(header, "header");
        this.body = Objects.requireNonNull(body, "body");
        this.closeAction = Objects.requireNonNull(closeAction, "closeAction");
    }

    public Optional<RoutingId> routingId() {
        return routingId;
    }

    public Message header() {
        return header;
    }

    public Message body() {
        return body;
    }

    @Override
    public void close() {
        synchronized (this) {
            if (closed) {
                return;
            }
            closed = true;
        }
        closeAction.run();
    }
}
