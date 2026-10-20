package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaDurableInitialPeerTest {
    @Test
    void initialAbsenceWaitsForTheFirstPeerRouteInsteadOfEndingItsLifecycle() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "initial-peer");
                var target = new ZLinkJavaRawMeshNode(context, "initial-peer")) {
            source.setRoutingId(RoutingId.from("initial-peer-source"));
            source.setBind("inproc://initial-peer-source-" + java.util.UUID.randomUUID());
            source.start();
            target.setRoutingId(RoutingId.from("new-durable-peer"));
            target.setBind("inproc://initial-peer-target-" + java.util.UUID.randomUUID());
            target.start();
            var initialObserved = new CompletableFuture<Void>();
            var submitted = new AtomicInteger();
            var reply = List.of(new byte[] {9});
            var request =
                    ZLinkJavaDurableRequest.request(
                                    () -> List.of(new byte[] {1}),
                                    (frames, remaining, admission) -> {
                                        submitted.incrementAndGet();
                                        admission.complete(null);
                                        return CompletableFuture.completedFuture(reply);
                                    },
                                    () -> source.observePeerRoute(target.routingId()),
                                    (changed, ended) ->
                                            source.onPeerStateChanged(
                                                    target.routingId(),
                                                    () -> {
                                                        changed.run();
                                                        runOwner(
                                                                source,
                                                                () ->
                                                                        initialObserved.complete(
                                                                                null));
                                                    },
                                                    ended),
                                    action -> runOwner(source, action),
                                    Duration.ofSeconds(5))
                            .toCompletableFuture();
            try {
                initialObserved.get(3, TimeUnit.SECONDS);
                assertFalse(
                        request.isDone(),
                        "A target never observed or removed must await its first ready route");
                assertEquals(0, submitted.get());
                source.preparePeerConnectionAsync(
                                target.advertisedEndpoint(),
                                target.routingId(),
                                target.lifecycleGeneration(),
                                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                                Duration.ofSeconds(2))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
                assertSame(reply, request.get(3, TimeUnit.SECONDS));
                assertEquals(1, submitted.get());
            } finally {
                source.close();
                request.handle((value, error) -> null).join();
            }
        }
    }

    static java.util.concurrent.CompletionStage<Void> runOwner(
            ZLinkJavaRawMeshNode source, Runnable action) {
        return source.runDescriptorTurn(action);
    }
}
