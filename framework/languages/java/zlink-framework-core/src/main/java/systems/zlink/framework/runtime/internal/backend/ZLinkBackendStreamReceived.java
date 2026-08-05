package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

/**
 * One raw STREAM receive result owned by the framework receive loop.
 *
 * <p>The message list is the binding's ordered multipart view. Each message
 * keeps its native {@link Message#more()} flag so the caller can distinguish
 * the final part without entering the binding's private receive machinery.</p>
 */
public final class ZLinkBackendStreamReceived implements AutoCloseable {
    private final Optional<RoutingId> routingId;
    private final List<Message> parts;
    private final Runnable closeAction;
    private boolean closed;

    public ZLinkBackendStreamReceived(
        Optional<RoutingId> routingId,
        List<Message> parts,
        Runnable closeAction) {
        this.routingId = routingId == null ? Optional.empty() : routingId;
        this.parts = Objects.requireNonNull(parts, "parts");
        this.closeAction = Objects.requireNonNull(closeAction, "closeAction");
    }

    public Optional<RoutingId> routingId() {
        return routingId;
    }

    public List<Message> parts() {
        return parts;
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
