package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

/** Owns command 42/43 sealing and one-way command 44 submission. */
public final class ZLinkSessionRelocationPeerClient {
    private static final Duration[] RETRANSMIT_INTERVALS = {
        Duration.ofSeconds(1),
        Duration.ofSeconds(1),
        Duration.ofSeconds(2),
        Duration.ofSeconds(4),
    };
    private static final Duration STEADY_RETRANSMIT_INTERVAL =
        Duration.ofSeconds(5);

    private final ZLinkInternalMeshNode node;
    private final ZLinkServiceM6BWireCodec codec;

    public ZLinkSessionRelocationPeerClient(ZLinkInternalMeshNode node) {
        this(node, new ZLinkServiceM6BWireCodec());
    }

    ZLinkSessionRelocationPeerClient(
        ZLinkInternalMeshNode node,
        ZLinkServiceM6BWireCodec codec) {
        this.node = Objects.requireNonNull(node, "node");
        this.codec = Objects.requireNonNull(codec, "codec");
    }

    public CompletionStage<Void> sendRoute(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        Objects.requireNonNull(command, "command");
        return node.sendSessionRelocationRoute(
            command.session().nodeRid(),
            codec.encodeSessionRelocationRoute(command));
    }

    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
        sealRouteUntilAck(
            ZLinkServiceM6BWireCodec.SessionRelocationSeal command,
            Duration deadline) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(deadline, "deadline");
        if (deadline.isZero() || deadline.isNegative()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "Session relocation seal deadline must be positive"));
        }
        byte[] command42 = codec.encodeSessionRelocationSeal(command);
        CompletableFuture<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            settled = new CompletableFuture<>();
        attemptSeal(
            command,
            command42,
            System.nanoTime() + deadline.toNanos(),
            settled,
            0);
        return settled;
    }

    static Duration retransmitInterval(int attempt) {
        return attempt < RETRANSMIT_INTERVALS.length
            ? RETRANSMIT_INTERVALS[attempt]
            : STEADY_RETRANSMIT_INTERVAL;
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
        Duration window = remaining.compareTo(interval) < 0
            ? remaining
            : interval;
        long sentAt = System.nanoTime();
        node.requestSessionRelocationSeal(
                command.session().nodeRid(), command42, window)
            .thenApply(codec::decodeSessionRelocationSealed)
            .whenComplete((sealed, failure) -> {
                if (failure == null) {
                    if (sealed.echoes(command)) {
                        settled.complete(sealed);
                    } else {
                        settled.completeExceptionally(
                            new ZLinkConfigurationException(
                                "command 43 differs from command 42"));
                    }
                    return;
                }
                Duration delay = interval.minusNanos(
                    System.nanoTime() - sentAt);
                ZLinkActorRetryScheduler.scheduleRouteAfter(
                    () -> attemptSeal(
                        command,
                        command42,
                        deadlineNanos,
                        settled,
                        attempt + 1),
                    delay);
            });
    }
}
