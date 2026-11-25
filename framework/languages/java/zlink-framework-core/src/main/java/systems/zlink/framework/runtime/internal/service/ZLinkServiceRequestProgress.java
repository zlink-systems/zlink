package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;

import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.function.Consumer;
import java.util.function.Function;

/** Observes request progress without owning admission, dispatch or completion. */
public class ZLinkServiceRequestProgress {
    public static final ZLinkServiceRequestProgress NONE = new ZLinkServiceRequestProgress();

    @FunctionalInterface
    public interface InstanceReply extends Consumer<List<Message>> {
        InstanceReply DISCARD = parts -> parts.forEach(Message::close);

        default ZLinkServiceRequestProgress progress() {
            return NONE;
        }
    }

    public void routeSelected(
            String meshName, RoutingId target, long correlation, long generation, boolean local) {}

    public void peerAdmitted() {}

    public void bindingSubmitted() {}

    public void handlerStarted() {}

    public void replyReceived() {}

    public <T> Function<List<Message>, T> replyDecoder(Function<List<Message>, T> decodeReply) {
        return decodeReply;
    }

    public CompletableFuture<Void> localAdmission(CompletableFuture<Void> admission) {
        return admission;
    }

    public InstanceReply localReply(ZLinkServiceOperationRegistry registry, UUID id) {
        return parts -> completeLocal(registry, id, parts);
    }

    private static void completeLocal(
            ZLinkServiceOperationRegistry registry, UUID id, List<Message> parts) {
        if (!registry.complete(id, parts)) parts.forEach(Message::close);
    }

    public CompletableFuture<Void> bindingAdmission(CompletableFuture<Void> admission) {
        return admission;
    }

    public ZLinkBackendReceived receiveInstance(
            RoutingId source,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
            byte[] metadata,
            List<Message> parts,
            InstanceReply reply,
            String contentType) {
        return new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.of(source),
                Optional.ofNullable(header.sourceSpotId()),
                Optional.ofNullable(header.replyRouteId()),
                metadata,
                new byte[0],
                parts,
                header.request() ? reply : null,
                null,
                contentType);
    }

    static final class Recording extends ZLinkServiceRequestProgress {
        private String meshName;
        private RoutingId target;
        private long correlation;
        private long generation;
        private boolean local;
        private final long started = System.nanoTime();
        private volatile long routeSelected;
        private volatile long peerAdmitted;
        private volatile long bindingSubmitted;
        private volatile long bindingAdmitted;
        private volatile long localDispatch;
        private volatile long replyReceived;

        @Override
        public void routeSelected(
                String meshName,
                RoutingId target,
                long correlation,
                long generation,
                boolean local) {
            this.meshName = meshName;
            this.target = target;
            this.correlation = correlation;
            this.generation = generation;
            this.local = local;
            routeSelected = elapsed();
        }

        @Override
        public void peerAdmitted() {
            if (peerAdmitted == 0) peerAdmitted = elapsed();
        }

        @Override
        public void bindingSubmitted() {
            bindingSubmitted = elapsed();
        }

        @Override
        public void handlerStarted() {
            if (localDispatch == 0) localDispatch = elapsed();
        }

        @Override
        public void replyReceived() {
            replyReceived = elapsed();
        }

        @Override
        public <T> Function<List<Message>, T> replyDecoder(Function<List<Message>, T> decodeReply) {
            return reply -> {
                replyReceived();
                return decodeReply.apply(reply);
            };
        }

        @Override
        public CompletableFuture<Void> localAdmission(CompletableFuture<Void> admission) {
            return observeAdmission(admission, this::handlerStarted);
        }

        @Override
        public InstanceReply localReply(ZLinkServiceOperationRegistry registry, UUID id) {
            return new InstanceReply() {
                @Override
                public void accept(List<Message> parts) {
                    replyReceived();
                    completeLocal(registry, id, parts);
                }

                @Override
                public ZLinkServiceRequestProgress progress() {
                    return Recording.this;
                }
            };
        }

        @Override
        public CompletableFuture<Void> bindingAdmission(CompletableFuture<Void> admission) {
            return observeAdmission(admission, () -> bindingAdmitted = elapsed());
        }

        private static CompletableFuture<Void> observeAdmission(
                CompletableFuture<Void> admission, Runnable admitted) {
            var observed = new CompletableFuture<Void>();
            observed.whenComplete(
                    (ignored, failure) -> {
                        if (failure == null) {
                            admitted.run();
                            if (admission != null) admission.complete(null);
                        } else if (admission != null) {
                            admission.completeExceptionally(failure);
                        }
                    });
            return observed;
        }

        @Override
        public ZLinkBackendReceived receiveInstance(
                RoutingId source,
                ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
                byte[] metadata,
                List<Message> parts,
                InstanceReply reply,
                String contentType) {
            return new ObservedInstance(source, header, metadata, parts, reply, contentType, this);
        }

        long elapsed() {
            return Math.max(1, System.nanoTime() - started);
        }

        void report(long terminal) {
            long selected = beforeTerminal(routeSelected, terminal);
            long peer = beforeTerminal(peerAdmitted, terminal);
            long submitted = beforeTerminal(bindingSubmitted, terminal);
            long admitted = beforeTerminal(bindingAdmitted, terminal);
            long dispatched = beforeTerminal(localDispatch, terminal);
            long reply = beforeTerminal(replyReceived, terminal);
            String stage;
            if (reply != 0) stage = "reply_completion";
            else if (admitted != 0 || dispatched != 0) stage = "reply_wait";
            else if (submitted != 0) stage = "binding_admission";
            else if (peer != 0) stage = local ? "local_dispatch" : "binding_submit";
            else stage = "peer_admission";

            System.err.println(
                    """
                    [zlink.runtime.relocation] node_request_deadline {
                      meshName: %s,
                      targetRid: %s,
                      correlationId: '%s',
                      stage: '%s',
                      routeGeneration: %sn,
                      elapsedMs: %s,
                      routeSelectedMs: %s,
                      peerAdmittedMs: %s,
                      bindingSubmittedMs: %s,
                      bindingAdmittedMs: %s,
                      replyReceivedMs: %s,
                      localDispatchMs: %s
                    }\
                    """
                            .formatted(
                                    selected == 0 ? "undefined" : quoted(meshName),
                                    selected == 0 ? "undefined" : quoted(target.toString()),
                                    selected == 0 ? "0" : Long.toUnsignedString(correlation),
                                    stage,
                                    selected == 0 ? "0" : Long.toUnsignedString(generation),
                                    millis(terminal),
                                    millis(selected),
                                    millis(peer),
                                    millis(submitted),
                                    millis(admitted),
                                    millis(reply),
                                    millis(dispatched)));
        }

        private static long beforeTerminal(long time, long terminal) {
            return time <= terminal ? time : 0;
        }

        private static String millis(long nanos) {
            return nanos == 0 ? "undefined" : Double.toString(nanos / 1_000_000.0);
        }

        private static String quoted(String value) {
            return "'"
                    + value.replace("\\", "\\\\")
                            .replace("'", "\\'")
                            .replace("\n", "\\n")
                            .replace("\r", "\\r")
                            .replace("\t", "\\t")
                    + "'";
        }
    }

    private static final class ObservedInstance extends ZLinkBackendReceived {
        private final Recording progress;

        private ObservedInstance(
                RoutingId source,
                ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
                byte[] metadata,
                List<Message> parts,
                InstanceReply reply,
                String contentType,
                Recording progress) {
            super(
                    ZLinkBackendRequestResult.OK,
                    Optional.of(source),
                    Optional.ofNullable(header.sourceSpotId()),
                    Optional.ofNullable(header.replyRouteId()),
                    metadata,
                    new byte[0],
                    parts,
                    reply,
                    null,
                    contentType);
            this.progress = progress;
        }

        @Override
        public ZLinkServiceRequestProgress requestProgress() {
            return progress;
        }
    }
}
