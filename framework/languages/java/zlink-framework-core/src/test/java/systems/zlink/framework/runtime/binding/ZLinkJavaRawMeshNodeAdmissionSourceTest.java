package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawMeshNodeAdmissionSourceTest {
    @Test
    void topologyAdmissionRejectionSignalsTheTargetObserverOnce() throws Exception {
        var localRid = RoutingId.from("rejected-observer");
        var peerRid = RoutingId.from("rejected-peer");
        var ioDeadline = Duration.ofSeconds(1);
        var wire = new ZLinkServiceM6AWireCodec();
        try (var context = Zlink.createContext();
                var localPort = new ZLinkJavaRawServicePort(context);
                var peerPort = new ZLinkJavaRawServicePort(context)) {
            var localRouter = localPort.openRouter(localRid);
            var peerRouter = peerPort.openRouter(peerRid);
            localRouter.options().probe(true);
            peerRouter.options().probe(true);
            var endpoint = "inproc://rejected-observer-" + System.nanoTime();
            localRouter.bind(endpoint);
            peerRouter.connect(endpoint);
            localPort.ensureReceivePollerRegistered(localRouter);
            peerPort.ensureReceivePollerRegistered(peerRouter);
            long routeDeadline = System.nanoTime() + ioDeadline.toNanos();
            var routes = localPort.routesSnapshot(localRouter);
            while (routes.stream().noneMatch(route -> route.routingId().equals(peerRid))
                    && System.nanoTime() < routeDeadline) {
                Thread.onSpinWait();
                routes = localPort.routesSnapshot(localRouter);
            }
            long generation =
                    routes.stream()
                            .filter(route -> route.routingId().equals(peerRid))
                            .findFirst()
                            .orElseThrow(() -> new AssertionError("expected Core selected route"))
                            .routeGeneration();
            var local =
                    new ZLinkServiceNodeDescriptor(
                            "mesh",
                            localRid,
                            1,
                            1,
                            endpoint,
                            List.of(),
                            ZLinkServiceNodeDescriptor.State.SERVING,
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            1,
                            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                            ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                            1,
                            1,
                            100,
                            0,
                            0);
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
            var topology = new ZLinkServiceTopologyRegistry(local, localPort.stateLane());
            try (var source =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "mesh",
                            localPort,
                            localRouter,
                            local,
                            topology,
                            Map.of(peerRid, generation),
                            Runnable::run)) {
                var observed = new AtomicInteger();
                var initial = new CompletableFuture<Void>();
                var rejected = new CompletableFuture<Void>();
                try (var registration =
                        source.onPeerStateChanged(
                                peerRid,
                                () -> {
                                    if (observed.incrementAndGet() == 1) initial.complete(null);
                                    else rejected.complete(null);
                                },
                                () -> {})) {
                    initial.get(1, TimeUnit.SECONDS);
                    assertEquals(1, observed.get());
                    for (int attempt = 0; attempt < 2; attempt++) {
                        peerPort.send(
                                        peerRouter,
                                        localRid,
                                        List.of(
                                                wire.encodeAdmission(
                                                        ServiceWireConstants.COMMAND_HELLO, peer)))
                                .toCompletableFuture()
                                .get(1, TimeUnit.SECONDS);
                        long helloDeadline = System.nanoTime() + ioDeadline.toNanos();
                        while (true) {
                            long remaining = helloDeadline - System.nanoTime();
                            assertTrue(remaining > 0, "expected HELLO before deadline");
                            assertTrue(
                                    localPort.waitForReadable(
                                            localRouter, Duration.ofNanos(remaining)));
                            var inbound = localPort.receive(localRouter);
                            assertNotNull(inbound);
                            if (inbound.frames().getFirst().length == 0) {
                                inbound.close();
                                continue;
                            }
                            try {
                                assertEquals(peerRid, inbound.source());
                                assertEquals(generation, inbound.routeGeneration());
                                assertEquals(
                                        ServiceWireConstants.COMMAND_HELLO,
                                        wire.decodeHeader(inbound.frames().getFirst()).command());
                            } catch (Throwable failure) {
                                inbound.close();
                                throw failure;
                            }
                            source.dispatch(inbound);
                            break;
                        }
                        long rejectDeadline = System.nanoTime() + ioDeadline.toNanos();
                        while (true) {
                            long remaining = rejectDeadline - System.nanoTime();
                            assertTrue(remaining > 0, "expected REJECT before deadline");
                            assertTrue(
                                    peerPort.waitForReadable(
                                            peerRouter, Duration.ofNanos(remaining)));
                            try (var rejection = peerPort.receive(peerRouter)) {
                                assertNotNull(rejection);
                                if (rejection.frames().getFirst().length == 0) continue;
                                assertEquals(localRid, rejection.source());
                                assertEquals(1, rejection.frames().size());
                                assertEquals(
                                        ServiceWireConstants.COMMAND_REJECT,
                                        wire.decodeHeader(rejection.frames().getFirst()).command());
                                wire.decodeReject(rejection.frames().getFirst());
                                break;
                            }
                        }
                        if (attempt == 0) rejected.get(1, TimeUnit.SECONDS);
                        assertTrue(topology.peer(peerRid).isEmpty());
                        assertFalse(topology.isReady(peerRid));
                        assertEquals(0, source.readyPeerCount());
                        // The rejection branch gates publication with rejectedPeers.add;
                        // this count observation is not a fence for every async callback.
                        assertEquals(2, observed.get());
                    }
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
                            "onPeerStateChanged", RoutingId.class, Runnable.class, Runnable.class);
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
                                                    },
                                            (Runnable) () -> {})) {
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
