package systems.zlink.framework.runtime.internal.diagnostics;

import java.security.SecureRandom;
import java.time.Instant;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.Executor;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
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
