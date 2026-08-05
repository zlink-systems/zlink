package systems.zlink.framework.runtime.streams;

import java.util.Objects;
import systems.zlink.contracts.messaging.Message;

/** Owns the two messages produced by one complete STREAM frame. */
final class ZLinkStreamInboundFrame implements AutoCloseable {
    private Message header;
    private Message payload;

    ZLinkStreamInboundFrame(Message header, Message payload) {
        this.header = Objects.requireNonNull(header, "header");
        this.payload = Objects.requireNonNull(payload, "payload");
    }

    Message header() {
        return header;
    }

    Message payload() {
        return payload;
    }

    @Override
    public void close() {
        Message currentHeader = header;
        Message currentPayload = payload;
        header = null;
        payload = null;
        if (currentHeader != null) {
            currentHeader.close();
        }
        if (currentPayload != null) {
            currentPayload.close();
        }
    }
}
