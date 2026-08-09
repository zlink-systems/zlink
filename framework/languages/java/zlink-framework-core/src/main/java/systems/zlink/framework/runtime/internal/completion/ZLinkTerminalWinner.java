package systems.zlink.framework.runtime.internal.completion;
import java.util.Objects;

import java.util.concurrent.atomic.AtomicReference;

/** Gives one terminal cause exclusive ownership of an operation completion. */
public final class ZLinkTerminalWinner {
    public enum Cause {
        RESPONSE,
        TIMEOUT,
        CANCELLATION,
        SHUTDOWN,
        DISCONNECT,
        FAILURE
    }

    private final AtomicReference<Cause> winner = new AtomicReference<>();

    public boolean tryWin(Cause cause) {
        return winner.compareAndSet(null, Objects.requireNonNull(cause, "cause"));
    }

    public boolean isTerminal() {
        return winner.get() != null;
    }

    public Cause winner() {
        return winner.get();
    }
}
