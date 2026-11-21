package systems.zlink.framework.streams;

import java.util.Objects;
import java.util.Optional;

public record ZLinkStreamError(
        ZLinkStreamSessionError error,
        String message,
        Optional<ZLinkDisconnectReason> disconnectReason) {
    public ZLinkStreamError {
        Objects.requireNonNull(disconnectReason, "disconnectReason");
    }

    public ZLinkStreamError(ZLinkStreamSessionError error, String message) {
        this(error, message, Optional.empty());
    }
}
