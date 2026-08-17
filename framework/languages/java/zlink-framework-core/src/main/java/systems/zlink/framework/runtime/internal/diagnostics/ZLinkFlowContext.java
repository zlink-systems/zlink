package systems.zlink.framework.runtime.internal.diagnostics;

import java.security.SecureRandom;
import java.time.Instant;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.Executor;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

/** Internal flow context. It is deliberately not a public context-capture API. */
public final class ZLinkFlowContext {
    private static final SecureRandom RANDOM = new SecureRandom();
    private static final AtomicLong LAST_MILLIS = new AtomicLong();
    private static final ThreadLocal<State> CURRENT = new ThreadLocal<>();

    private ZLinkFlowContext() {
    }

    public static State current() {
        return CURRENT.get();
    }

    public static ThreadLocal<State> threadLocal() {
        return CURRENT;
    }

    public static State create(ZLinkFlowOrigin origin) {
        return new State(uuidV7(), origin);
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
     * Ingress entry: installs the inbound flow pair, or starts a new flow when
     * the message carries none (spec 27 §4). Callers must decode the inbound
     * pair only when capture is enabled and use {@link #suppress()} otherwise.
     */
    public static Scope enterOrCreate(State inbound, ZLinkFlowOrigin defaultOrigin) {
        return enter(inbound != null ? inbound : create(defaultOrigin));
    }

    /**
     * Outbound entry: preserves an ambient callback flow or starts an
     * application flow for the first outbound operation (spec 27 §4). The
     * live capture gate is intentionally checked by every terminal entrypoint.
     */
    public static Scope enterCurrentOrCreate(
        ZLinkFlowOrigin origin,
        boolean captureEnabled) {
        return captureEnabled
            ? enterOrCreate(current(), origin)
            : suppress();
    }

    /**
     * Off path (spec 27 §4): no validation, no context capture, no new flow.
     * A stale ambient flow left by a scope entered while tracing was enabled
     * is cleared for the duration so outbound encoders do not copy it onto
     * envelopes; steady-state Off pays only the ambient null read.
     */
    public static Scope suppress() {
        State previous = CURRENT.get();
        if (previous == null) {
            return NOOP;
        }
        CURRENT.set(null);
        return () -> CURRENT.set(previous);
    }

    private static final Scope NOOP = () -> { };

    public static Executor propagating(Executor delegate) {
        return command -> {
            State captured = current();
            delegate.execute(() -> {
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
        source.whenComplete((value, error) -> {
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
        if (value == null || value.length() != 36
            || value.charAt(8) != '-'
            || value.charAt(13) != '-'
            || value.charAt(18) != '-'
            || value.charAt(23) != '-'
            || value.charAt(14) != '7') {
            return false;
        }
        char variant = value.charAt(19);
        if (variant != '8' && variant != '9'
            && variant != 'a' && variant != 'b') {
            return false;
        }
        for (int index = 0; index < value.length(); index++) {
            if (index == 8 || index == 13 || index == 18 || index == 23) {
                continue;
            }
            char current = value.charAt(index);
            if (!((current >= '0' && current <= '9')
                || (current >= 'a' && current <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    private static String uuidV7() {
        long now = Instant.now().toEpochMilli();
        long timestamp = LAST_MILLIS.updateAndGet(previous -> Math.max(now, previous));
        long randomA = RANDOM.nextLong() & 0x0fffL;
        long msb = ((timestamp & 0xffffffffffffL) << 16) | 0x7000L | randomA;
        long lsb = (RANDOM.nextLong() & 0x3fffffffffffffffL) | 0x8000000000000000L;
        return new UUID(msb, lsb).toString();
    }

    public record State(String flowId, ZLinkFlowOrigin origin) {
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
