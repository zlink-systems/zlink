package systems.zlink.framework.runtime.internal.diagnostics;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.time.Instant;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BiFunction;
import java.util.function.Supplier;

/** Internal flow context. It is deliberately not a public context-capture API. */
public final class ZLinkFlowContext {
    public static final int FLOW_ID_TEXT_LENGTH = 36;
    private static final String LEGACY_FRAME_PREFIX = "__zlink.flow\n";
    private static final int FIRST_HYPHEN_POSITION = 8;
    private static final int SECOND_HYPHEN_POSITION = 13;
    private static final int THIRD_HYPHEN_POSITION = 18;
    private static final int FOURTH_HYPHEN_POSITION = 23;
    private static final int VERSION_POSITION = SECOND_HYPHEN_POSITION + 1;
    private static final int VARIANT_POSITION = THIRD_HYPHEN_POSITION + 1;
    private static final int UUID_VERSION = 7;
    private static final char VERSION_DIGIT = (char) ('0' + UUID_VERSION);
    private static final int TIMESTAMP_BITS = 48;
    private static final int RANDOM_A_BITS = 12;
    private static final long TIMESTAMP_MASK = (1L << TIMESTAMP_BITS) - 1;
    private static final long RANDOM_A_MASK = (1L << RANDOM_A_BITS) - 1;
    private static final long VERSION_MASK = (long) UUID_VERSION << RANDOM_A_BITS;
    private static final long VARIANT_RANDOM_MASK = Long.MAX_VALUE >>> 1;
    private static final SecureRandom RANDOM = new SecureRandom();
    private static final AtomicLong LAST_MILLIS = new AtomicLong();
    private static final ThreadLocal<State> CURRENT = new ThreadLocal<>();

    private ZLinkFlowContext() {}

    public static Message encodeLegacyFrame(State state) {
        return state == null
                ? null
                : Message.from(
                        (LEGACY_FRAME_PREFIX + state.flowId() + "\n" + state.origin().name())
                                .getBytes(StandardCharsets.UTF_8));
    }

    public static State decodeLegacyFrame(
            String value,
            String surface,
            BiFunction<String, Throwable, ? extends RuntimeException> errorFactory) {
        if (!value.startsWith(LEGACY_FRAME_PREFIX)) {
            return null;
        }
        String[] fields = value.split("\n", -1);
        if (fields.length != 3 || fields[1].isBlank()) {
            throw errorFactory.apply(surface + " flow fields are malformed", null);
        }
        if (!isValidFlowId(fields[1])) {
            throw errorFactory.apply(surface + " flow id must be UUIDv7", null);
        }
        try {
            return new State(fields[1], ZLinkFlowOrigin.valueOf(fields[2]), null);
        } catch (IllegalArgumentException invalidOrigin) {
            throw errorFactory.apply(surface + " flow origin is invalid", invalidOrigin);
        }
    }

    public static State current() {
        return CURRENT.get();
    }

    public static ThreadLocal<State> threadLocal() {
        return CURRENT;
    }

    public static State create(ZLinkFlowOrigin origin) {
        return new State(uuidV7(), origin, null);
    }

    public static Scope enter(State state) {
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

    /**
     * Ingress entry: installs the inbound flow pair, or starts a new flow when the message carries
     * none (spec 27 §4). Callers must decode the inbound pair only when capture is enabled and use
     * {@link #suppress()} otherwise.
     */
    public static Scope enterOrCreate(State inbound, ZLinkFlowOrigin defaultOrigin) {
        return enter(inbound != null ? inbound : create(defaultOrigin));
    }

    /**
     * Outbound entry: preserves an ambient callback flow or starts an application flow for the
     * first outbound operation (spec 27 §4). The live capture gate is intentionally checked by
     * every terminal entrypoint.
     */
    public static Scope enterCurrentOrCreate(ZLinkFlowOrigin origin, boolean captureEnabled) {
        return captureEnabled ? enterOrCreate(current(), origin) : suppress();
    }

    /**
     * Off path (spec 27 §4): no validation, no context capture, no new flow. A stale ambient flow
     * left by a scope entered while tracing was enabled is cleared for the duration so outbound
     * encoders do not copy it onto envelopes; steady-state Off pays only the ambient null read.
     */
    public static Scope suppress() {
        State previous = CURRENT.get();
        if (previous == null) {
            return NOOP;
        }
        CURRENT.set(null);
        return () -> CURRENT.set(previous);
    }

    private static final Scope NOOP = () -> {};

    public static Executor propagating(Executor delegate) {
        return command -> {
            State captured = current();
            delegate.execute(
                    () -> {
                        if (captured == null) {
                            command.run();
                            return;
                        }
                        try (Scope ignored = enter(captured)) {
                            command.run();
                        }
                    });
        };
    }

    public static <T> CompletionStage<T> propagate(CompletionStage<T> source) {
        State captured = current();
        if (captured == null) return source;
        CompletableFuture<T> result = new CompletableFuture<>();
        source.whenComplete(
                (value, error) -> {
                    try (Scope ignored = enter(captured)) {
                        if (error != null) result.completeExceptionally(error);
                        else result.complete(value);
                    }
                });
        return result;
    }

    public static void run(State state, Runnable action) {
        if (state == null) {
            action.run();
            return;
        }
        try (Scope ignored = enter(state)) {
            action.run();
        }
    }

    public static <T> T call(State state, Supplier<T> action) {
        if (state == null) {
            return action.get();
        }
        try (Scope ignored = enter(state)) {
            return action.get();
        }
    }

    /** Lowercase hyphenated UUIDv7 wire validation from spec 27 §3. */
    public static boolean isValidFlowId(String value) {
        if (value == null
                || value.length() != FLOW_ID_TEXT_LENGTH
                || value.charAt(FIRST_HYPHEN_POSITION) != '-'
                || value.charAt(SECOND_HYPHEN_POSITION) != '-'
                || value.charAt(THIRD_HYPHEN_POSITION) != '-'
                || value.charAt(FOURTH_HYPHEN_POSITION) != '-'
                || value.charAt(VERSION_POSITION) != VERSION_DIGIT) {
            return false;
        }
        char variant = value.charAt(VARIANT_POSITION);
        if (variant != '8' && variant != '9' && variant != 'a' && variant != 'b') {
            return false;
        }
        for (int index = 0; index < value.length(); index++) {
            if (index == FIRST_HYPHEN_POSITION
                    || index == SECOND_HYPHEN_POSITION
                    || index == THIRD_HYPHEN_POSITION
                    || index == FOURTH_HYPHEN_POSITION) {
                continue;
            }
            char current = value.charAt(index);
            if (!((current >= '0' && current <= '9') || (current >= 'a' && current <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    private static String uuidV7() {
        long now = Instant.now().toEpochMilli();
        long timestamp = LAST_MILLIS.updateAndGet(previous -> Math.max(now, previous));
        long randomA = RANDOM.nextLong() & RANDOM_A_MASK;
        long msb =
                ((timestamp & TIMESTAMP_MASK) << (Long.SIZE - TIMESTAMP_BITS))
                        | VERSION_MASK
                        | randomA;
        long lsb = (RANDOM.nextLong() & VARIANT_RANDOM_MASK) | Long.MIN_VALUE;
        return new UUID(msb, lsb).toString();
    }

    public record State(String flowId, ZLinkFlowOrigin origin, String streamSessionId) {
        public State {
            if (flowId == null || origin == null) {
                throw new IllegalArgumentException("flow id and origin are required");
            }
        }
    }

    @FunctionalInterface
    public interface Scope extends AutoCloseable {
        @Override
        void close();
    }
}
