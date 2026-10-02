package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Supplier;

final class ZLinkJavaRawMeshAuthorityCompletionTest {
    @Test
    void actorAuthorityCompletionEntersApplicationQueueOutsideProviderThread() throws Exception {
        assertApplicationHandoff(true);
    }

    @Test
    void spotAuthorityCompletionEntersApplicationQueueOutsideProviderThread() throws Exception {
        assertApplicationHandoff(false);
    }

    private static void assertApplicationHandoff(boolean actorRequest) throws Exception {
        var callbackRegistered = new CompletableFuture<Void>();
        var callbackTerminal = new AtomicReference<CompletableFuture<?>>();
        var provider = new TrackingFuture<Void>(callbackRegistered, callbackTerminal);
        var queried = new CompletableFuture<Void>();
        var dispatchThread = new CompletableFuture<Thread>();
        var queries = new AtomicInteger();
        var dispatches = new AtomicInteger();
        var targetQueue = queue();
        RoutingId sourceRid = RoutingId.from("authority-completion-source");
        RoutingId targetRid = RoutingId.from("authority-completion-target");
        String endpoint = "inproc://authority-completion-" + System.nanoTime();
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setApplicationJobQueue(queue());
            target.setApplicationJobQueue(targetQueue);
            source.setRoutingId(sourceRid);
            source.setBind("inproc://authority-completion-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) -> {
                        var result =
                                provider.thenApply(
                                        ignored ->
                                                Optional.of(
                                                        new ZLinkInternalMeshNode
                                                                .PeerAuthorityFence(
                                                                rid, generation, "owner", 1)));
                        if (queries.incrementAndGet() == 2) queried.complete(null);
                        return result;
                    });
            target.spotNode()
                    .setRelocationStagingIngressHandler(
                            new ZLinkInternalSpotNode.RelocationStagingIngressHandler() {
                                @Override
                                public boolean handleSpot(
                                        ZLinkInternalMeshNode.PeerAuthorityFence fence,
                                        ZLinkServiceM6BWireCodec.SpotMessage header,
                                        byte[] metadata,
                                        Supplier<byte[]> record,
                                        int size,
                                        List<Message> parts,
                                        String contentType,
                                        Consumer<List<Message>> reply,
                                        Consumer<Throwable> failure) {
                                    dispatchThread.complete(Thread.currentThread());
                                    dispatches.incrementAndGet();
                                    return false;
                                }

                                @Override
                                public boolean handleActor(
                                        ZLinkInternalMeshNode.PeerAuthorityFence fence,
                                        ZLinkServiceM6BWireCodec.ActorMessage header,
                                        Supplier<byte[]> record,
                                        List<Message> parts,
                                        String contentType,
                                        Consumer<List<Message>> reply,
                                        Consumer<Throwable> failure) {
                                    dispatchThread.complete(Thread.currentThread());
                                    dispatches.incrementAndGet();
                                    return false;
                                }
                            });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            ZLinkJavaRawMeshNodeM6ATest.awaitAdmitted(source);
            CompletableFuture<?> request;
            try (var payload = Message.from("request")) {
                if (actorRequest) {
                    var actor = new ZLinkBackendActorRef(targetRid, "missing-actor", 1);
                    source.spotNode().rememberActorAuthority(actor, 1, 1);
                    request =
                            source.requestActor(actor, List.of(payload), Duration.ofSeconds(5))
                                    .toCompletableFuture();
                } else {
                    ((ZLinkJavaRawSpotNode) source.spotNode())
                            .rememberSpotAuthority(targetRid, "missing-spot", 1, 1, 1);
                    request =
                            source.requestSpot(
                                            "source-spot",
                                            targetRid,
                                            "missing-spot",
                                            1,
                                            new byte[0],
                                            List.of(payload),
                                            Duration.ofSeconds(5))
                                    .toCompletableFuture();
                }
            }
            queried.get(5, TimeUnit.SECONDS);
            callbackRegistered.get(5, TimeUnit.SECONDS);
            Thread providerThread = Thread.currentThread();
            provider.complete(null);
            Thread observed = dispatchThread.get(5, TimeUnit.SECONDS);
            callbackTerminal.get().get(5, TimeUnit.SECONDS);
            if (actorRequest) {
                assertThrows(ExecutionException.class, () -> request.get(5, TimeUnit.SECONDS));
            } else {
                try (var reply = (ZLinkBackendReceived) request.get(5, TimeUnit.SECONDS)) {
                    assertEquals(ZLinkBackendRequestResult.NOT_FOUND, reply.result());
                }
            }
            assertEquals(2, queries.get());
            assertEquals(1, dispatches.get());
            assertEquals(0, targetQueue.snapshot().queuedApplicationJobs());
            assertNotSame(
                    providerThread,
                    observed,
                    "authority provider completion must not execute application ingress");
        }
    }

    private static ZLinkApplicationJobQueue queue() {
        return new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.empty(),
                new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
    }

    private static final class TrackingFuture<T> extends CompletableFuture<T> {
        private final CompletableFuture<Void> callbackRegistered;
        private final AtomicReference<CompletableFuture<?>> callbackTerminal;

        private TrackingFuture(
                CompletableFuture<Void> callbackRegistered,
                AtomicReference<CompletableFuture<?>> callbackTerminal) {
            this.callbackRegistered = callbackRegistered;
            this.callbackTerminal = callbackTerminal;
        }

        @Override
        public <U> CompletableFuture<U> newIncompleteFuture() {
            return new TrackingFuture<>(callbackRegistered, callbackTerminal);
        }

        @Override
        public CompletableFuture<T> whenComplete(BiConsumer<? super T, ? super Throwable> action) {
            var result = super.whenComplete(action);
            callbackTerminal.set(result);
            callbackRegistered.complete(null);
            return result;
        }

        @Override
        public CompletableFuture<T> whenCompleteAsync(
                BiConsumer<? super T, ? super Throwable> action, Executor executor) {
            var result = super.whenCompleteAsync(action, executor);
            callbackTerminal.set(result);
            callbackRegistered.complete(null);
            return result;
        }
    }
}
