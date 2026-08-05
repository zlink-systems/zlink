package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
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

    CompletionStage<Void> switchRouteThenOpenAdmission(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        Duration timeout,
        SteadyNormalizer normalization,
        AdmissionGate admission) {
        Objects.requireNonNull(command, "command");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(normalization, "normalization");
        Objects.requireNonNull(admission, "admission");
        admission.close();
        return switchRoute(command, timeout)
            .thenCompose(ack -> normalization.normalize(command, ack))
            .thenRun(admission::open);
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
        Throwable current = error;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZlinkRequestException request) {
            return request.getResult() == RequestResult.NOT_CONNECTED;
        }
        return current instanceof ZlinkSubmitException submit
            && submit.getResult() == SubmitResult.NOT_CONNECTED;
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

    @FunctionalInterface
    interface SteadyNormalizer {
        CompletionStage<Void> normalize(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
            ZLinkServiceM6BWireCodec.SessionRelocationRouted ack);
    }

    interface AdmissionGate {
        void close();

        void open();
    }
}
