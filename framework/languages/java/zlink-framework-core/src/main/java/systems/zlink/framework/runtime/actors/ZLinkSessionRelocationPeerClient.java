package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

/** Owns the command 44/45 route-switch barrier at the target Actor owner. */
public final class ZLinkSessionRelocationPeerClient {
    private final ZLinkInternalMeshNode node;
    private final ZLinkServiceM6BWireCodec codec;
    private volatile BiConsumer<
        ZLinkServiceM6BWireCodec.SessionRelocationRoute,
        ZLinkServiceM6BWireCodec.SessionRelocationRouted>
            routeTerminalObserver = (ignoredCommand, ignoredAck) -> { };

    public ZLinkSessionRelocationPeerClient(ZLinkInternalMeshNode node) {
        this(node, new ZLinkServiceM6BWireCodec());
    }

    ZLinkSessionRelocationPeerClient(
        ZLinkInternalMeshNode node,
        ZLinkServiceM6BWireCodec codec) {
        this.node = Objects.requireNonNull(node, "node");
        this.codec = Objects.requireNonNull(codec, "codec");
    }

    void setRouteTerminalObserver(BiConsumer<
        ZLinkServiceM6BWireCodec.SessionRelocationRoute,
        ZLinkServiceM6BWireCodec.SessionRelocationRouted> observer) {
        routeTerminalObserver = observer == null
            ? (ignoredCommand, ignoredAck) -> { }
            : observer;
    }

    //  Spec 20 §5 step 8 and §5.1: the first retransmission happens 1s after
    //  the first send, then at 1s, 2s, 4s, 5s and 5s from then on. Each send
    //  uses the interval that precedes the next one as its response window so
    //  a lost `sessionActorLocationUpdateResMsg` is retransmitted exactly on
    //  that schedule instead of after the caller's whole deadline.
    private static final Duration[] RETRANSMIT_INTERVALS = {
        Duration.ofSeconds(1),
        Duration.ofSeconds(1),
        Duration.ofSeconds(2),
        Duration.ofSeconds(4),
    };
    private static final Duration STEADY_RETRANSMIT_INTERVAL =
        Duration.ofSeconds(5);

    /**
     * Switches one bound-Session route and keeps retransmitting on the spec
     * schedule in the background until a termination condition holds: a valid
     * command 45 ACK, a session-owner stale-binding rejection (the relocation
     * was superseded — terminal no-op), or the caller-supplied superseded
     * check (session-owner lease expiry, node lifecycle or Actor authority
     * generations advanced past this relocation). The returned stage
     * settles once the first attempt resolves so relocation finalize
     * converges without gating admission on any single route.
     */
    public CompletionStage<Void> switchRouteUntilTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        Supplier<CompletionStage<Boolean>> superseded) {
        return switchRouteUntilTerminal(
            command, timeout, superseded, (ack, failure) -> { });
    }

    /**
     * Same background convergence, with {@code onTerminal} notified once the
     * retransmission loop stops. The returned stage settles after the first
     * attempt regardless of outcome, so it can never report whether the route
     * actually switched; {@code onTerminal} carries the command 45 ACK, or the
     * failure that proved this relocation superseded.
     */
    public CompletionStage<Void> switchRouteUntilTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        Supplier<CompletionStage<Boolean>> superseded,
        BiConsumer<
            ZLinkServiceM6BWireCodec.SessionRelocationRouted,
            Throwable> onTerminal) {
        return switchRouteUntilTerminal(
            command,
            timeout,
            superseded,
            () -> CompletableFuture.completedFuture(null),
            onTerminal);
    }

    /**
     * Adds a durable pre-send step that is repeated before every identical
     * command 44 retransmission. Runtime recovery uses it to renew the root
     * that owns the exact route context until command 45 is terminal.
     */
    public CompletionStage<Void> switchRouteUntilTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        Supplier<CompletionStage<Boolean>> superseded,
        Supplier<CompletionStage<Void>> beforeAttempt,
        BiConsumer<
            ZLinkServiceM6BWireCodec.SessionRelocationRouted,
            Throwable> onTerminal) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(superseded, "superseded");
        Objects.requireNonNull(beforeAttempt, "beforeAttempt");
        Objects.requireNonNull(onTerminal, "onTerminal");
        CompletableFuture<Void> settled = new CompletableFuture<>();
        attemptRoute(
            command,
            timeout,
            superseded,
            beforeAttempt,
            onTerminal,
            settled,
            0);
        return settled;
    }

    static Duration retransmitInterval(int attempt) {
        return attempt < RETRANSMIT_INTERVALS.length
            ? RETRANSMIT_INTERVALS[attempt]
            : STEADY_RETRANSMIT_INTERVAL;
    }

    private void attemptRoute(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        Supplier<CompletionStage<Boolean>> superseded,
        Supplier<CompletionStage<Void>> beforeAttempt,
        BiConsumer<
            ZLinkServiceM6BWireCodec.SessionRelocationRouted,
            Throwable> onTerminal,
        CompletableFuture<Void> settled,
        int attempt) {
        Duration interval = retransmitInterval(attempt);
        Duration window = timeout.compareTo(interval) < 0 ? timeout : interval;
        long sentAt = System.nanoTime();
        CompletionStage<Void> ready;
        try {
            ready = Objects.requireNonNull(
                beforeAttempt.get(), "route pre-send stage");
        } catch (RuntimeException failure) {
            ready = CompletableFuture.failedFuture(failure);
        }
        ready.thenCompose(ignored -> switchRoute(command, window))
            .whenComplete((ack, failure) -> {
            if (failure == null) {
                notifyTerminal(command, onTerminal, ack, failure);
                settled.complete(null);
                return;
            }
            CompletionStage<Boolean> check;
            try {
                check = Objects.requireNonNull(
                    superseded.get(), "superseded check stage");
            } catch (RuntimeException unavailable) {
                check = CompletableFuture.completedFuture(false);
            }
            check.whenComplete((terminal, error) -> {
                settled.complete(null);
                if (error == null && Boolean.TRUE.equals(terminal)) {
                    notifyTerminal(command, onTerminal, null, failure);
                    return;
                }
                //  The retransmission clock is anchored at the previous send,
                //  so an attempt that failed early still waits out the spec
                //  interval instead of hot-looping on the session owner.
                Duration remaining = interval.minusNanos(
                    System.nanoTime() - sentAt);
                ZLinkActorRetryScheduler.scheduleRouteAfter(
                    () -> attemptRoute(
                        command,
                        timeout,
                        superseded,
                        beforeAttempt,
                        onTerminal,
                        settled,
                        attempt + 1),
                    remaining);
            });
        });
    }

    private void notifyTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        BiConsumer<
            ZLinkServiceM6BWireCodec.SessionRelocationRouted,
            Throwable> onTerminal,
        ZLinkServiceM6BWireCodec.SessionRelocationRouted ack,
        Throwable failure) {
        if (ack != null) {
            try {
                routeTerminalObserver.accept(command, ack);
            } catch (RuntimeException ignoredRecovery) {
                //  The observer owns its durable retry; diagnostics below must
                //  still receive the terminal command 45.
            }
        }
        try {
            onTerminal.accept(ack, failure);
        } catch (RuntimeException ignoredDiagnostic) {
            // Diagnostics never change route convergence.
        }
    }

    /**
     * Sends command 42 and returns the Session owner's command 43 ACK,
     * retransmitting the identical seal on the spec 20 §5 step 8 schedule
     * until the ACK arrives or {@code deadline} elapses. The schema fixes the
     * loss policy for command 42 as `retransmit-until-sealed-or-deadline`, so
     * the deadline is mandatory: the caller has already sealed the Actor lane
     * and must not wait on an unreachable owner forever. Retransmitting the
     * identical seal is safe — the owner answers a byte-identical retransmit
     * from its cached terminal without re-sealing.
     */
    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
        sealRouteUntilAck(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal command,
            Duration deadline) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(deadline, "deadline");
        byte[] command42 = codec.encodeSessionRelocationSeal(command);
        CompletableFuture<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            settled = new CompletableFuture<>();
        attemptSeal(command, command42,
            System.nanoTime() + deadline.toNanos(), settled, 0);
        return settled;
    }

    private void attemptSeal(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command,
        byte[] command42,
        long deadlineNanos,
        CompletableFuture<
            ZLinkServiceM6BWireCodec.SessionRelocationSealed> settled,
        int attempt) {
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0) {
            settled.completeExceptionally(new ZLinkConfigurationException(
                "Session relocation seal deadline elapsed before command 43"));
            return;
        }
        Duration remaining = Duration.ofNanos(remainingNanos);
        Duration interval = retransmitInterval(attempt);
        Duration window =
            remaining.compareTo(interval) < 0 ? remaining : interval;
        long sentAt = System.nanoTime();
        node.requestSessionRelocationSeal(
                command.session().nodeRid(), command42, window)
            .thenApply(codec::decodeSessionRelocationSealed)
            .whenComplete((ack, failure) -> {
                if (failure == null) {
                    if (ack.echoes(command)) {
                        settled.complete(ack);
                        return;
                    }
                    //  An ACK that does not echo the seal is terminal: the
                    //  owner answered a different fence, so retransmitting the
                    //  same bytes can never succeed.
                    settled.completeExceptionally(
                        new ZLinkConfigurationException(
                            "command 43 ACK differs from the command 42 fence"));
                    return;
                }
                //  The retransmission clock is anchored at the previous send,
                //  so an attempt that failed early still waits out the spec
                //  interval instead of hot-looping on the Session owner.
                Duration delay = interval.minusNanos(
                    System.nanoTime() - sentAt);
                ZLinkActorRetryScheduler.scheduleRouteAfter(
                    () -> attemptSeal(
                        command, command42, deadlineNanos, settled,
                        attempt + 1),
                    delay);
            });
    }

    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        switchRoute(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            Duration timeout) {
        return switchRoute(command, command.lastAcceptedSessionSequence(), timeout);
    }

    /**
     * Sends a source abort until the Session owner acknowledges the exact
     * command-42 high-water or the caller's deadline elapses. The abort wire
     * body deliberately carries zero, so the expected high-water must come
     * from the matching command-43 context retained by the source.
     */
    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        abortRouteUntilAck(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            long expectedHighWater,
            Duration deadline) {
        return abortRouteUntilAck(
            command, expectedHighWater, deadline, () -> false);
    }

    CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        abortRouteUntilAck(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            long expectedHighWater,
            Duration deadline,
            BooleanSupplier stopped) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(deadline, "deadline");
        Objects.requireNonNull(stopped, "stopped");
        if (command.action()
                != ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT
            || expectedHighWater < 0) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "Session relocation abort context is invalid"));
        }
        byte[] command44 = codec.encodeSessionRelocationRoute(command);
        CompletableFuture<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
            settled = new CompletableFuture<>();
        attemptAbort(
            command,
            command44,
            expectedHighWater,
            System.nanoTime() + deadline.toNanos(),
            stopped,
            settled,
            0);
        return settled;
    }

    private void attemptAbort(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        byte[] command44,
        long expectedHighWater,
        long deadlineNanos,
        BooleanSupplier stopped,
        CompletableFuture<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
            settled,
        int attempt) {
        if (settled.isDone()) {
            return;
        }
        if (stopped.getAsBoolean()) {
            settled.completeExceptionally(new ZLinkConfigurationException(
                "Session relocation abort stopped before command 45"));
            return;
        }
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0) {
            settled.completeExceptionally(new ZLinkConfigurationException(
                "Session relocation abort deadline elapsed before command 45"));
            return;
        }
        Duration remaining = Duration.ofNanos(remainingNanos);
        Duration interval = retransmitInterval(attempt);
        Duration window =
            remaining.compareTo(interval) < 0 ? remaining : interval;
        long sentAt = System.nanoTime();
        requestRoute(command, command44, expectedHighWater, window)
            .whenComplete((ack, failure) -> {
                if (failure == null) {
                    settled.complete(ack);
                    return;
                }
                if (stopped.getAsBoolean()) {
                    settled.completeExceptionally(
                        new ZLinkConfigurationException(
                            "Session relocation abort stopped before command 45"));
                    return;
                }
                Duration delay = interval.minusNanos(
                    System.nanoTime() - sentAt);
                ZLinkActorRetryScheduler.scheduleRouteAfter(
                    () -> attemptAbort(
                        command,
                        command44,
                        expectedHighWater,
                        deadlineNanos,
                        stopped,
                        settled,
                        attempt + 1),
                    delay);
            });
    }

    private CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        switchRoute(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            long expectedHighWater,
            Duration timeout) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(timeout, "timeout");
        byte[] command44 = codec.encodeSessionRelocationRoute(command);
        return requestRoute(command, command44, expectedHighWater, timeout);
    }

    private CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        requestRoute(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            byte[] command44,
            long expectedHighWater,
            Duration timeout) {
        return ZLinkActorRetryScheduler.retryRouteUntil(
                timeout,
                () -> node.requestSessionRelocationRoute(
                        command.session().nodeRid(), command44, timeout)
                    .thenApply(codec::decodeSessionRelocationRouted),
                ZLinkSessionRelocationPeerClient::isRouteNotConnected)
            .thenCompose(ack -> validateAck(command, expectedHighWater, ack)
                ? CompletableFuture.completedFuture(ack)
                : CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "command 45 ACK differs from command 44 fence")));
    }

    private static boolean isRouteNotConnected(Throwable error) {
        Throwable current = unwrap(error);
        if (current instanceof ZlinkRequestException request) {
            return request.getResult() == RequestResult.NOT_CONNECTED;
        }
        return current instanceof ZlinkSubmitException submit
            && submit.getResult() == SubmitResult.NOT_CONNECTED;
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static boolean validateAck(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        long expectedHighWater,
        ZLinkServiceM6BWireCodec.SessionRelocationRouted ack) {
        boolean exactCommand = ack.relocation().equals(command.relocation())
            && ack.coordinator().equals(command.coordinator())
            && ack.actor().equals(command.actor())
            && ack.session().equals(command.session())
            && ack.action() == command.action();
        if (!exactCommand) {
            return false;
        }
        //  A refusal reports the owner's current state rather than echoing the
        //  requested fence. Its identity and action still close this exact
        //  retransmission, while only a successful result can prove that the
        //  requested authority and accepted high-water were applied.
        return switch (ack.result()) {
            case APPLIED, ALREADY_APPLIED ->
                ack.currentAuthorityOwnerGeneration()
                    == command.currentAuthorityOwnerGeneration()
                && ack.lastAcceptedSessionSequence() == expectedHighWater;
            case STALE, SESSION_OR_BINDING_CLOSED -> true;
        };
    }

}
