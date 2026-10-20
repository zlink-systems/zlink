package systems.zlink.framework.runtime.internal.dispatch;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;

/**
 * Carries one host queue reservation across Framework-owned serial and executor boundaries. The
 * context is an ownership carrier only; all admission and accounting policy remains in {@link
 * ZLinkApplicationJobQueue}.
 */
public final class ZLinkApplicationJobContext {
    private static final ThreadLocal<State> CURRENT = new ThreadLocal<>();
    // Restores an empty context; shared because it captures nothing.
    private static final Scope CLEAR = CURRENT::remove;

    private ZLinkApplicationJobContext() {}

    /** Enters the receive/claim reservation scope. */
    public static Scope enter(ZLinkApplicationJobQueue.Permit permit) {
        return enter(
                new State(Objects.requireNonNull(permit, "permit"), new QueuedOwnership(permit)));
    }

    /** Returns the reservation currently carried by this Framework turn. */
    public static Optional<ZLinkApplicationJobQueue.Permit> current() {
        State state = CURRENT.get();
        return state == null ? Optional.empty() : Optional.ofNullable(state.permit);
    }

    /**
     * Returns whether the current ingress reservation has not yet crossed a Framework queue
     * boundary. A queued handler keeps its permit in this context after handler entry, but cannot
     * transfer it to a second job.
     */
    public static boolean hasTransferableQueuedOwnership() {
        State state = CURRENT.get();
        return state != null && state.ownership != null && state.ownership.canHandoff();
    }

    /**
     * Transfers the current receive reservation to exactly one queued job. Repeated queue captures
     * in the same ingress scope cannot duplicate a permit; fan-out must acquire one distinct permit
     * for each job.
     */
    public static QueuedOwnership transferToQueuedJob() {
        QueuedOwnership ownership = transferQueuedOwnership();
        if (ownership != null) ownership.markQueued();
        return ownership;
    }

    /** Transfers queued ownership without entering the host permit owner. */
    public static QueuedOwnership transferQueuedOwnership() {
        State state = CURRENT.get();
        if (state == null || state.ownership == null) return null;
        return state.ownership.handoff();
    }

    /** Re-enters a permit already owned by a queued Framework job. */
    public static Scope enterQueued(QueuedOwnership ownership) {
        return ownership == null
                ? () -> {}
                : enter(new State(ownership.currentPermit(), ownership));
    }

    /** Test/direct-invocation bridge for a permit already marked queued. */
    public static Scope enterQueued(ZLinkApplicationJobQueue.Permit permit) {
        return permit == null ? () -> {} : enter(new State(permit, new QueuedOwnership(permit)));
    }

    /**
     * Returns capacity immediately before the first application instruction. The returned permit
     * stays in this context but no longer moves to another job.
     */
    public static void beforeFirstApplicationInstruction() {
        State state = CURRENT.get();
        if (state != null) {
            QueuedOwnership ownership = state.ownership;
            state.ownership = null;
            if (ownership != null) ownership.close();
        }
    }

    private static Scope enter(State next) {
        State previous = CURRENT.get();
        CURRENT.set(next);
        return previous == null ? CLEAR : () -> CURRENT.set(previous);
    }

    @FunctionalInterface
    public interface Scope extends AutoCloseable {
        @Override
        void close();
    }

    /** One queue/executor boundary's exact ownership of a queued permit. */
    public static final class QueuedOwnership implements AutoCloseable {
        private static final VarHandle HANDED_OFF;
        private static final VarHandle RESERVATION;

        static {
            try {
                HANDED_OFF =
                        MethodHandles.lookup()
                                .findVarHandle(QueuedOwnership.class, "handedOff", boolean.class);
                RESERVATION =
                        MethodHandles.lookup()
                                .findVarHandle(QueuedOwnership.class, "reservation", Object.class);
            } catch (ReflectiveOperationException failure) {
                throw new ExceptionInInitializerError(failure);
            }
        }

        private final ZLinkApplicationJobQueue owner;
        private volatile boolean handedOff;
        // Null is durable payload ownership without a runnable reservation. A pending
        // future is the host's cancellable waiter, not an execution-gate claim.
        private volatile Object reservation;

        private QueuedOwnership(ZLinkApplicationJobQueue.Permit permit) {
            this(permit.owner(), permit);
        }

        private QueuedOwnership(ZLinkApplicationJobQueue owner, Object reservation) {
            this.owner = owner;
            this.reservation = reservation;
        }

        private QueuedOwnership handoff() {
            return HANDED_OFF.compareAndSet(this, false, true)
                    ? new QueuedOwnership(owner, reservation)
                    : null;
        }

        private boolean canHandoff() {
            return !handedOff;
        }

        private ZLinkApplicationJobQueue.Permit currentPermit() {
            var current = reservation;
            if (current instanceof ZLinkApplicationJobQueue.Permit permit) return permit;
            if (current instanceof CompletableFuture<?> future
                    && future.isDone()
                    && !future.isCompletedExceptionally())
                return (ZLinkApplicationJobQueue.Permit) future.getNow(null);
            return null;
        }

        /** Accounts an accepted queued job on its host permit owner. */
        public void markQueued() {
            var permit = currentPermit();
            if (permit != null) permit.queued();
        }

        /** Returns the initial reservation when the owner FIFO takes durable payload ownership. */
        public boolean takeDurable() {
            Object current = reservation;
            return current instanceof ZLinkApplicationJobQueue.Permit
                    && RESERVATION.compareAndSet(this, current, Optional.of(current));
        }

        public boolean isDurable() {
            return !(reservation instanceof ZLinkApplicationJobQueue.Permit);
        }

        /** Host bookkeeping runs after the owner committed the position, outside its monitor. */
        public Object settleAdmission() {
            Object current = reservation;
            if (current instanceof Optional<?> detached) {
                if (RESERVATION.compareAndSet(this, current, null)) {
                    release(detached.orElseThrow());
                    return detached.orElseThrow();
                }
            } else markQueued();
            return null;
        }

        public boolean isRunnable() {
            Object current = reservation;
            return current instanceof ZLinkApplicationJobQueue.Permit
                    || current instanceof CompletableFuture<?> waiting && waiting.isDone();
        }

        /**
         * Registers capacity outside the serial monitor and without claiming its execution gate.
         */
        public void prepareRunnable(Runnable ready) {
            if (handedOff || reservation != null) return;
            var claim = new CompletableFuture<ZLinkApplicationJobQueue.Permit>();
            if (!RESERVATION.compareAndSet(this, null, claim)) return;
            if (handedOff) {
                releaseReservation();
                return;
            }
            var acquisition =
                    owner.acquire(ZLinkApplicationJobQueue.Origin.LOCAL).toCompletableFuture();
            claim.whenComplete(
                    (permit, failure) -> {
                        if (claim.isCancelled()) acquisition.cancel(false);
                        ready.run();
                    });
            acquisition.whenComplete(
                    (permit, failure) -> {
                        if (failure != null) claim.completeExceptionally(failure);
                        else if (!claim.complete(permit)) permit.close();
                    });
        }

        /** Observes only an already completed head acquisition outside the serial monitor. */
        public void requireRunnable() {
            if (reservation instanceof CompletableFuture<?> future) future.join();
            markQueued();
        }

        /** Detaches a head claim atomically; the caller settles it outside its serial monitor. */
        public void suspendRunnable() {
            while (true) {
                Object current = reservation;
                if (current == null || current instanceof Optional<?>) return;
                if (RESERVATION.compareAndSet(this, current, Optional.of(current))) return;
            }
        }

        private void releaseReservation() {
            release(RESERVATION.getAndSet(this, null));
        }

        private static void release(Object current) {
            if (current instanceof ZLinkApplicationJobQueue.Permit permit) permit.close();
            else if (current instanceof Optional<?> detached) release(detached.orElseThrow());
            else if (current instanceof CompletableFuture<?> waiting && !waiting.cancel(false))
                waiting.thenAccept(permit -> ((ZLinkApplicationJobQueue.Permit) permit).close());
        }

        @Override
        public void close() {
            if (HANDED_OFF.compareAndSet(this, false, true)) {
                releaseReservation();
            }
        }
    }

    /** Carries already durable payloads into their existing FIFO without an ingress reservation. */
    public static Scope enterDurable(ZLinkApplicationJobQueue owner) {
        return enter(new State(null, new QueuedOwnership(Objects.requireNonNull(owner), null)));
    }

    private static final class State {
        private final ZLinkApplicationJobQueue.Permit permit;
        private QueuedOwnership ownership;

        private State(ZLinkApplicationJobQueue.Permit permit, QueuedOwnership ownership) {
            this.permit = permit;
            this.ownership = ownership;
        }
    }
}
