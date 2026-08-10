package systems.zlink.framework.messaging;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

public final class ZLinkMessage {
    private static final ZLinkEncodedPayload EMPTY_PAYLOAD =
        ZLinkEncodedPayload.from(new byte[0]);

    private final Object value;
    private final ZLinkEncodedPayload encodedPayload;
    private final ZLinkMessageSerializer serializer;
    private final Class<?> declaredType;
    private final boolean empty;
    private final AtomicReference<CompletableFuture<DecodeOutcome>> decoded =
        new AtomicReference<>();

    private ZLinkMessage(
        Object value,
        ZLinkEncodedPayload encodedPayload,
        ZLinkMessageSerializer serializer,
        Class<?> declaredType,
        boolean empty) {
        this.value = value;
        this.encodedPayload = encodedPayload;
        this.serializer = serializer;
        this.declaredType = declaredType;
        this.empty = empty;
    }

    public static ZLinkMessage empty() {
        return new ZLinkMessage(null, EMPTY_PAYLOAD, null, ZLinkMessage.class, true);
    }

    public static ZLinkMessage of(Object value) {
        if (value instanceof ZLinkMessage message) {
            return message;
        }
        return new ZLinkMessage(
            Objects.requireNonNull(value, "value"),
            null,
            null,
            value.getClass(),
            false);
    }

    /**
     * Wraps a value and preserves the type declared by the caller for outbound codec selection.
     *
     * @throws IllegalArgumentException when {@code value} is not an instance of
     *     {@code declaredType}
     */
    public static ZLinkMessage of(Object value, Class<?> declaredType) {
        Objects.requireNonNull(value, "value");
        Objects.requireNonNull(declaredType, "declaredType");
        if (!declaredType.isInstance(value)) {
            throw new IllegalArgumentException(
                "value of type " + value.getClass().getName()
                    + " is not an instance of declared type "
                    + declaredType.getName());
        }
        return new ZLinkMessage(value, null, null, declaredType, false);
    }

    public static ZLinkMessage fromEncoded(ZLinkEncodedPayload payload, ZLinkMessageSerializer serializer) {
        Objects.requireNonNull(payload, "payload");
        Objects.requireNonNull(serializer, "serializer");
        return new ZLinkMessage(
            null,
            payload,
            serializer,
            ZLinkMessage.class,
            payload.bytes().length == 0);
    }

    public boolean isEmpty() {
        return empty;
    }

    /** Returns the type used to select the outbound codec for this message. */
    public Class<?> declaredType() {
        return declaredType;
    }

    public <T> T decode(Class<T> type) {
        Objects.requireNonNull(type, "type");
        if (value != null) {
            if (type.isInstance(value)) {
                return type.cast(value);
            }
        }
        CompletableFuture<DecodeOutcome> selected = decoded.get();
        if (selected == null) {
            CompletableFuture<DecodeOutcome> candidate = new CompletableFuture<>();
            if (decoded.compareAndSet(null, candidate)) {
                try {
                    ZLinkEncodedPayload encoded = value == null
                        ? encodedPayload
                        : serializerForEncode().serialize(value, declaredType);
                    candidate.complete(new Decoded(
                        type,
                        serializerForDecode().deserialize(encoded, type)));
                } catch (Throwable failure) {
                    candidate.complete(new DecodeFailed(failure));
                }
                selected = candidate;
            } else {
                selected = decoded.get();
            }
        }
        DecodeOutcome outcome = selected.join();
        if (outcome instanceof DecodeFailed failed) {
            throw propagate(failed.failure());
        }
        Decoded materialized = (Decoded) outcome;
        if (materialized.type() != type) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TYPE_MISMATCH,
                "Message was decoded as " + materialized.type().getName()
                    + " and cannot be decoded again as " + type.getName());
        }
        return type.cast(materialized.value());
    }

    public ZLinkEncodedPayload toEncodedPayload(ZLinkMessageSerializer serializer) {
        Objects.requireNonNull(serializer, "serializer");
        if (value != null) {
            return serializer.serialize(value, declaredType);
        }
        return encodedPayload;
    }

    private ZLinkMessageSerializer serializerForDecode() {
        if (serializer == null) {
            throw new IllegalStateException("message does not have a codec registry for decode");
        }
        return serializer;
    }

    private ZLinkMessageSerializer serializerForEncode() {
        if (serializer != null) {
            return serializer;
        }
        throw new IllegalStateException("message does not have a codec registry for re-encode");
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
            "message decode failed",
            failure);
    }

    private sealed interface DecodeOutcome permits Decoded, DecodeFailed {
    }

    private record Decoded(Class<?> type, Object value) implements DecodeOutcome {
    }

    private record DecodeFailed(Throwable failure) implements DecodeOutcome {
    }
}
