package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

/** Durable terminal replayed only to the exact creation operation. */
public record ZLinkCreationOperationTerminal(
        ZLinkCreationOperationIdentity operation,
        ZLinkObjectReservation reservation,
        ZLinkCreationTerminalState state,
        byte[] terminalEnvelope,
        Instant expiresAt) {
    public static final int MAX_REPLY_ENVELOPE_SIZE = 1024 * 1024;

    public ZLinkCreationOperationTerminal {
        Objects.requireNonNull(operation, "operation");
        Objects.requireNonNull(reservation, "reservation");
        Objects.requireNonNull(state, "state");
        terminalEnvelope = Objects.requireNonNull(terminalEnvelope, "terminalEnvelope").clone();
        Objects.requireNonNull(expiresAt, "expiresAt");
        if (terminalEnvelope.length > MAX_REPLY_ENVELOPE_SIZE) {
            throw new IllegalArgumentException("terminalEnvelope must contain at most 1 MiB");
        }
    }

    @Override
    public byte[] terminalEnvelope() {
        return terminalEnvelope.clone();
    }
}
