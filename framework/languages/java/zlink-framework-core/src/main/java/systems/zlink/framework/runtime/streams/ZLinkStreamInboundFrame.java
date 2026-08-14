package systems.zlink.framework.runtime.streams;

import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.messaging.Message;

/** Owns the two messages produced by one complete STREAM frame. */
final class ZLinkStreamInboundFrame implements AutoCloseable {
    private Message header;
    private Message payload;
    private final Runnable terminalRelease;
    private final AtomicBoolean closed = new AtomicBoolean();

    ZLinkStreamInboundFrame(Message header, Message payload) {
        this(header, payload, () -> { });
    }

    ZLinkStreamInboundFrame(
        Message header,
        Message payload,
        Runnable terminalRelease) {
        this.header = Objects.requireNonNull(header, "header");
        this.payload = Objects.requireNonNull(payload, "payload");
        this.terminalRelease = Objects.requireNonNull(
            terminalRelease, "terminalRelease");
    }

    Message header() {
        return header;
    }

    Message payload() {
        return payload;
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        Message currentHeader = header;
        Message currentPayload = payload;
        header = null;
        payload = null;
        try {
            if (currentHeader != null) {
                currentHeader.close();
            }
            if (currentPayload != null) {
                currentPayload.close();
            }
        } finally {
            terminalRelease.run();
        }
    }
}
