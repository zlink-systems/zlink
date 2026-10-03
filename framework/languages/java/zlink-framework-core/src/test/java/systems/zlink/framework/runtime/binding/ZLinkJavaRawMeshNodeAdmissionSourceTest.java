package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawMeshNodeAdmissionSourceTest {
    @Test
    void topologyAdmissionRejectionSignalsTheTargetObserverOnce() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, "rejected-observer");
            var peerRid = RoutingId.from("rejected-peer");
            var descriptorField = ZLinkJavaRawMeshNode.class.getDeclaredField("localDescriptor");
            descriptorField.setAccessible(true);
            var local = (ZLinkServiceNodeDescriptor) descriptorField.get(source);
            var peer =
                    new ZLinkServiceNodeDescriptor(
                            "another-mesh",
                            peerRid,
                            local.lifecycleGeneration(),
                            local.descriptorRevision(),
                            local.advertisedEndpoint(),
                            local.channels(),
                            local.state(),
                            local.securityIdentity(),
                            local.applicationVersion(),
                            local.protocolCapabilities(),
                            local.objectRole(),
                            local.placementWeight(),
                            local.activeCapacityLimit(),
                            local.pendingCapacityLimit(),
                            local.activeCapacityUsed(),
                            local.pendingCapacityUsed());
            var observer =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "onPeerStateChanged", RoutingId.class, Runnable.class);
            observer.setAccessible(true);
            var admission =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "dispatchAdmission", ZLinkJavaRawServicePort.Inbound.class, int.class);
            admission.setAccessible(true);
            var laneField = ZLinkJavaRawMeshNode.class.getDeclaredField("descriptorStateLane");
            laneField.setAccessible(true);
            var lane =
                    (systems.zlink.framework.runtime.internal.execution.ZLinkStateLane)
                            laneField.get(source);
            var observed = new AtomicInteger();
            var initial = new CompletableFuture<Void>();
            var rejected = new CompletableFuture<Void>();
            try (var registration =
                    (AutoCloseable)
                            observer.invoke(
                                    source,
                                    peerRid,
                                    (Runnable)
                                            () -> {
                                                if (observed.incrementAndGet() == 1)
                                                    initial.complete(null);
                                                else rejected.complete(null);
                                            })) {
                initial.get(1, TimeUnit.SECONDS);
                int hello =
                        systems.zlink.framework.runtime.protocol.ServiceWireConstants.COMMAND_HELLO;
                var wire =
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6AWireCodec();
                try (var inbound =
                        new ZLinkJavaRawServicePort.Inbound(
                                peerRid,
                                null,
                                java.util.List.of(wire.encodeAdmission(hello, peer)),
                                new systems.zlink.contracts.messaging.Received())) {
                    lane.runAsync(
                                    () -> {
                                        try {
                                            admission.invoke(source, inbound, hello);
                                        } catch (ReflectiveOperationException failure) {
                                            throw new CompletionException(failure);
                                        }
                                        return null;
                                    })
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS);
                    rejected.get(1, TimeUnit.SECONDS);
                    lane.runAsync(
                                    () -> {
                                        try {
                                            admission.invoke(source, inbound, hello);
                                        } catch (ReflectiveOperationException failure) {
                                            throw new CompletionException(failure);
                                        }
                                        return null;
                                    })
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS);
                    lane.runAsync(() -> null).toCompletableFuture().get(1, TimeUnit.SECONDS);
                    assertEquals(2, observed.get());
                }
            }
        }
    }

    @Test
    void preparationOwnsDescriptorFenceAndReusesTheAdmittedIntent() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, "source");
            start(target, "target");
            source.preparePeerConnectionAsync(
                            target.advertisedEndpoint(),
                            target.routingId(),
                            target.lifecycleGeneration(),
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            var intents = source.connectionIntentIds();
            assertEquals(1, intents.size());
            var admitted =
                    source.peers().stream()
                            .filter(peer -> peer.routingId().equals(target.routingId()))
                            .findFirst()
                            .orElseThrow();
            assertEquals(target.lifecycleGeneration(), admitted.lifecycleGeneration());
            source.preparePeerConnectionAsync(
                            target.advertisedEndpoint(),
                            target.routingId(),
                            target.lifecycleGeneration(),
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            assertEquals(intents, source.connectionIntentIds());
        }
    }

    @Test
    void anAdmittedRidDoesNotAdmitAnIntentWithAnotherEndpointOrSecurityIdentity() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, "changed-fence-source");
            start(target, "changed-fence-target");
            source.preparePeerConnectionAsync(
                            target.advertisedEndpoint(),
                            target.routingId(),
                            target.lifecycleGeneration(),
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            var created = new CompletableFuture<Void>();
            try (var registration =
                    source.onStateChanged(
                            () -> {
                                if (source.connectionIntentIds().size() == 2)
                                    created.complete(null);
                            })) {
                var pending =
                        source.preparePeerConnectionAsync(
                                        "inproc://other-endpoint-" + System.nanoTime(),
                                        target.routingId(),
                                        target.lifecycleGeneration(),
                                        "another-security",
                                        Duration.ofSeconds(5))
                                .toCompletableFuture();
                created.get(1, TimeUnit.SECONDS);
                assertFalse(pending.isDone(), "the admitted RID has a different descriptor fence");
                assertTrue(pending.cancel(false));
            }
        }
    }

    @Test
    void wrongLifecycleCannotCompleteAdmissionAndCancelledWaiterDetaches() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, "fence-source");
            start(target, "fence-target");
            var rejected =
                    source.preparePeerConnectionAsync(
                                    target.advertisedEndpoint(),
                                    target.routingId(),
                                    target.lifecycleGeneration() ^ 1L,
                                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                                    Duration.ofMillis(100))
                            .toCompletableFuture();
            var failure =
                    assertThrows(ExecutionException.class, () -> rejected.get(2, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            var cancelled =
                    source.preparePeerConnectionAsync(
                                    "inproc://absent-" + System.nanoTime(),
                                    RoutingId.from("absent"),
                                    7,
                                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                                    Duration.ofSeconds(5))
                            .toCompletableFuture();
            assertTrue(cancelled.cancel(false));
            var observers = ZLinkJavaRawMeshNode.class.getDeclaredField("stateListeners");
            observers.setAccessible(true);
            assertTrue(((java.util.List<?>) observers.get(source)).isEmpty());
        }
    }

    @Test
    void observerFailureDoesNotHideAnotherObserverAndLocalChangesDoNotWakeTargetWaiter()
            throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, "observer-source");
            var initial = new CompletableFuture<Void>();
            var changed = new CompletableFuture<Void>();
            var count = new AtomicInteger();
            var targetCount = new AtomicInteger();
            var targetInitial = new CompletableFuture<Void>();
            var method =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "onPeerStateChanged", RoutingId.class, Runnable.class);
            method.setAccessible(true);
            try (var failing =
                            source.onStateChanged(
                                    () -> {
                                        throw new IllegalStateException("observer");
                                    });
                    var general =
                            source.onStateChanged(
                                    () -> {
                                        if (count.incrementAndGet() == 1) initial.complete(null);
                                        else changed.complete(null);
                                    });
                    var target =
                            (AutoCloseable)
                                    method.invoke(
                                            source,
                                            RoutingId.from("waiting-target"),
                                            (Runnable)
                                                    () -> {
                                                        targetCount.incrementAndGet();
                                                        targetInitial.complete(null);
                                                    })) {
                initial.get(1, TimeUnit.SECONDS);
                targetInitial.get(1, TimeUnit.SECONDS);
                source.setPlacementWeight(2);
                changed.get(1, TimeUnit.SECONDS);
                // Complete the same owner boundary used by observer publication.
                var laneField = ZLinkJavaRawMeshNode.class.getDeclaredField("descriptorStateLane");
                laneField.setAccessible(true);
                var lane =
                        (systems.zlink.framework.runtime.internal.execution.ZLinkStateLane)
                                laneField.get(source);
                lane.runAsync(() -> null).toCompletableFuture().get(1, TimeUnit.SECONDS);
                assertEquals(1, targetCount.get());
            }
        }
    }

    private static void start(ZLinkJavaRawMeshNode node, String id) {
        node.setRoutingId(RoutingId.from("audit-" + id));
        node.setBind("inproc://audit-" + id + "-" + System.nanoTime());
        node.start();
    }
}
