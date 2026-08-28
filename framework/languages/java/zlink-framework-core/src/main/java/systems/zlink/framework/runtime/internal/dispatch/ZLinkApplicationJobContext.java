package systems.zlink.framework.runtime.internal.dispatch;

import java.util.Objects;
import java.util.Optional;

/**
 * Carries one host queue reservation across Framework-owned serial and executor
 * boundaries. The context is an ownership carrier only; all admission and
 * accounting policy remains in {@link ZLinkApplicationJobQueue}.
 */
public final class ZLinkApplicationJobContext {
    private static final ThreadLocal<State> CURRENT = new ThreadLocal<>();

    private ZLinkApplicationJobContext() {
    }

    /** Enters the receive/claim reservation scope. */
    public static Scope enter(ZLinkApplicationJobQueue.Permit permit) {
        return enter(new State(
            Objects.requireNonNull(permit, "permit"),
            new QueuedOwnership(permit)));
    }

    /** Returns the reservation currently carried by this Framework turn. */
    public static Optional<ZLinkApplicationJobQueue.Permit> current() {
        State state = CURRENT.get();
        return state == null ? Optional.empty() : Optional.of(state.permit);
    }

    /**
     * Returns whether the current ingress reservation has not yet crossed a
     * Framework queue boundary. A queued handler keeps its permit in this
     * context after handler entry, but cannot transfer it to a second job.
     */
    public static boolean hasTransferableQueuedOwnership() {
        State state = CURRENT.get();
        return state != null && state.ownership.canHandoff();
    }

    /**
     * Transfers the current receive reservation to exactly one queued job.
     * Repeated queue captures in the same ingress scope cannot duplicate a
     * permit; fan-out must acquire one distinct permit for each job.
     */
    public static QueuedOwnership transferToQueuedJob() {
        State state = CURRENT.get();
        if (state == null || !state.ownership.handoff()) {
            return null;
        }
        state.permit.queued();
        return new QueuedOwnership(state.permit);
    }

    /** Re-enters a permit already owned by a queued Framework job. */
    public static Scope enterQueued(QueuedOwnership ownership) {
        return ownership == null
            ? () -> { }
            : enter(new State(ownership.permit, ownership));
    }

    /** Test/direct-invocation bridge for a permit already marked queued. */
    public static Scope enterQueued(ZLinkApplicationJobQueue.Permit permit) {
        return permit == null
            ? () -> { }
            : enter(new State(permit, new QueuedOwnership(permit)));
    }

    /** Returns capacity immediately before the first application instruction. */
    public static void beforeFirstApplicationInstruction() {
        State state = CURRENT.get();
        if (state != null) {
            state.permit.handlerStarted();
        }
    }

    private static Scope enter(State next) {
        State previous = CURRENT.get();
        CURRENT.set(next);
        return () -> {
            if (previous == null) {
                CURRENT.remove();
            } else {
                CURRENT.set(previous);
            }
        };
    }

    @FunctionalInterface
    public interface Scope extends AutoCloseable {
        @Override
        void close();
    }

    /** One queue/executor boundary's exact ownership of a queued permit. */
    public static final class QueuedOwnership implements AutoCloseable {
        private final ZLinkApplicationJobQueue.Permit permit;
        private boolean handedOff;

        private QueuedOwnership(ZLinkApplicationJobQueue.Permit permit) {
            this.permit = permit;
        }

        private synchronized boolean handoff() {
            if (handedOff) {
                return false;
            }
            handedOff = true;
            return true;
        }

        private synchronized boolean canHandoff() {
            return !handedOff;
        }

        @Override
        public void close() {
            boolean release;
            synchronized (this) {
                release = !handedOff;
                handedOff = true;
            }
            if (release) {
                permit.close();
            }
        }
    }

    private static final class State {
        private final ZLinkApplicationJobQueue.Permit permit;
        private final QueuedOwnership ownership;

        private State(
            ZLinkApplicationJobQueue.Permit permit,
            QueuedOwnership ownership) {
            this.permit = permit;
            this.ownership = ownership;
        }
    }
}
