package systems.zlink.framework.runtime.actors;
import java.util.concurrent.CompletionException;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
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

    public ZLinkSessionRelocationPeerClient(ZLinkInternalMeshNode node) {
        this(node, new ZLinkServiceM6BWireCodec());
    }

    ZLinkSessionRelocationPeerClient(
        ZLinkInternalMeshNode node,
        ZLinkServiceM6BWireCodec codec) {
        this.node = Objects.requireNonNull(node, "node");
        this.codec = Objects.requireNonNull(codec, "codec");
    }

    /**
     * Switches one bound-Session route and keeps retrying with backoff in
     * the background until a termination condition holds: a valid command 45
     * ACK, a session-owner stale-binding rejection (the relocation was
     * superseded — terminal no-op), or the caller-supplied superseded check
     * (session-owner lease expiry, node lifecycle or Actor authority
     * generations advanced past this relocation). The returned stage
     * settles once the first attempt resolves so relocation finalize
     * converges without gating admission on any single route.
     */
    public CompletionStage<Void> switchRouteUntilTerminal(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        Supplier<CompletionStage<Boolean>> superseded) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(superseded, "superseded");
        CompletableFuture<Void> settled = new CompletableFuture<>();
        attemptRoute(command, timeout, superseded, settled);
        return settled;
    }

    private void attemptRoute(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        Supplier<CompletionStage<Boolean>> superseded,
        CompletableFuture<Void> settled) {
        switchRoute(command, timeout).whenComplete((ack, failure) -> {
            if (failure == null || isRouteSuperseded(failure)) {
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
                    return;
                }
                ZLinkActorRetryScheduler.scheduleRoute(
                    () -> attemptRoute(command, timeout, superseded, settled));
            });
        });
    }

    public CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
        switchRoute(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            Duration timeout) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(timeout, "timeout");
        byte[] command44 = codec.encodeSessionRelocationRoute(command);
        return ZLinkActorRetryScheduler.retryRouteUntil(
                timeout,
                () -> node.requestSessionRelocationRoute(
                        command.session().nodeRid(), command44, timeout)
                    .thenApply(codec::decodeSessionRelocationRouted),
                ZLinkSessionRelocationPeerClient::isRouteNotConnected)
            .thenCompose(ack -> validateAck(command, ack)
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

    /**
     * A session-owner stale-binding rejection proves this relocation's
     * route command was superseded by a newer binding — terminal no-op.
     */
    static boolean isRouteSuperseded(Throwable error) {
        Throwable current = unwrap(error);
        return current instanceof ZLinkConfigurationException
            && current.getMessage() != null
            && current.getMessage().contains("stale binding fence");
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
        ZLinkServiceM6BWireCodec.SessionRelocationRouted ack) {
        return ack.relocation().equals(command.relocation())
            && ack.coordinator().equals(command.coordinator())
            && ack.actor().equals(command.actor())
            && ack.session().equals(command.session())
            && ack.action() == command.action()
            && ack.currentAuthorityOwnerGeneration()
                == command.currentAuthorityOwnerGeneration()
            && ack.lastAcceptedSessionSequence()
                == command.lastAcceptedSessionSequence();
    }

}
