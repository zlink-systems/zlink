package systems.zlink.framework.runtime.spots;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

/**
 * Owns the single typed decode outcome for one admitted inbound payload.
 * Transport storage remains owned by the surrounding dispatch until the
 * first decode completes; typed handler values never own that storage.
 */
final class ZLinkInboundPayloadOwner {
    private final Message payload;
    private final ZLinkMessageSerializer serializer;
    private final AtomicReference<CompletableFuture<DecodeOutcome>> outcome =
        new AtomicReference<>();

    ZLinkInboundPayloadOwner(
        Message payload,
        ZLinkMessageSerializer serializer) {
        this.payload = Objects.requireNonNull(payload, "payload");
        this.serializer = Objects.requireNonNull(serializer, "serializer");
    }

    Message rawView() {
        return payload;
    }

    Object deserialize(Class<?> declaredType) {
        Objects.requireNonNull(declaredType, "declaredType");
        CompletableFuture<DecodeOutcome> selected = outcome.get();
        if (selected == null) {
            CompletableFuture<DecodeOutcome> candidate = new CompletableFuture<>();
            if (outcome.compareAndSet(null, candidate)) {
                try {
                    Object value = ZLinkMessagePayloads.deserialize(
                        serializer, payload, declaredType);
                    candidate.complete(new Decoded(declaredType, value));
                } catch (Throwable failure) {
                    candidate.complete(new DecodeFailed(failure));
                }
                selected = candidate;
            } else {
                selected = outcome.get();
            }
        }

        DecodeOutcome resolved = selected.join();
        if (resolved instanceof DecodeFailed failed) {
            throw propagate(failed.failure());
        }
        Decoded decoded = (Decoded) resolved;
        if (decoded.declaredType() != declaredType) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TYPE_MISMATCH,
                "Inbound payload was decoded as " + decoded.declaredType().getName()
                    + " and cannot be decoded again as " + declaredType.getName());
        }
        return decoded.value();
    }

    private static RuntimeException propagate(Throwable failure) {
        if (failure instanceof RuntimeException runtimeFailure) {
            return runtimeFailure;
        }
        if (failure instanceof Error error) {
            throw error;
        }
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            "Inbound payload decode failed",
            failure);
    }

    private sealed interface DecodeOutcome permits Decoded, DecodeFailed {
    }

    private record Decoded(Class<?> declaredType, Object value)
        implements DecodeOutcome {
    }

    private record DecodeFailed(Throwable failure) implements DecodeOutcome {
    }
}
