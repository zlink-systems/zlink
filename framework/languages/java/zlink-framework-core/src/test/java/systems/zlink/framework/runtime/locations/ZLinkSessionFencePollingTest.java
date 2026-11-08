package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.binding.ZLinkSessionFenceTestHost;
import systems.zlink.framework.runtime.internal.locations.*;

import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.*;

final class ZLinkSessionFencePollingTest {
    private static ZLinkLocationRepository intercept(
            ZLinkLocationRepository repository, Function<Method, CompletionStage<?>> response) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, args) -> {
                            var result = response.apply(method);
                            if (result != null) return result;
                            try {
                                return method.invoke(repository, args);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    @RepeatedTest(100)
    void pollingReconcilesSessionCopyEvenWhenBackendAlreadyObservedTheActorLease()
            throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            f.expireAndClaim();
            f.publish(1);
            assertInstanceOf(
                    ZLinkAuthorityStored.class,
                    f.preserve(ZLinkAuthorityKeyCodec.actor("actor"), before));
            var snapshot =
                    (ZLinkAuthoritySnapshot)
                            f.repository
                                    .read(ZLinkAuthorityKeyCodec.actor("actor"), () -> false)
                                    .toCompletableFuture()
                                    .join();
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), snapshot)) {
                var sessionCopy = new java.util.concurrent.atomic.AtomicLong();
                var exactReads = new java.util.concurrent.atomic.AtomicInteger();
                host.sessionMeshNode()
                        .setSessionActorFenceHandler(
                                (node, socket, session, actor, binding, lease, publish) -> {
                                    if (publish.getAsBoolean()) sessionCopy.set(lease);
                                    return CompletableFuture.completedFuture(
                                            OptionalLong.of(sessionCopy.get()));
                                });
                var repository =
                        intercept(
                                f.repository,
                                method -> {
                                    if (method.getName().equals("read")
                                            || method.getName().equals("readOwnerLease"))
                                        exactReads.incrementAndGet();
                                    return null;
                                });
                var descriptors =
                        f.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                assertEquals(f.owner.leaseGeneration(), host.actorLease());
                host.poll(descriptors, repository);
                assertEquals(f.owner.leaseGeneration(), sessionCopy.get());
                assertEquals(2, exactReads.get());
                host.poll(descriptors, repository);
                assertEquals(
                        2, exactReads.get(), "unchanged copies must not repeat exact Store reads");
            }
        }
    }

    @RepeatedTest(100)
    void actorRecoveryDuringSessionLeaseReadKeepsBothLeaseUpdatesAndSequence() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            var owner =
                    new ZLinkP4RuntimeRecoveryTest.Fixture(
                            provider, RoutingId.from("session-node"));
            owner.expireAndClaim();
            owner.publish(1);
            var descriptors =
                    owner.repository
                            .listMeshNodes("game", ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items();
            var evidence = owner.repository.readOwnerLease("owner").toCompletableFuture().join();
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                var started = new CompletableFuture<Void>();
                var held = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                var repository =
                        intercept(
                                owner.repository,
                                method -> {
                                    if (!method.getName().equals("readOwnerLease")) return null;
                                    started.complete(null);
                                    return held;
                                });
                var polling = host.pollSessionOwnerAsync(descriptors, repository);
                started.orTimeout(5, TimeUnit.SECONDS).join();
                f.expireAndClaim();
                f.publish(1);
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor("actor"), before));
                host.recoverActorLease(f.owner.leaseGeneration());
                held.complete(evidence);
                polling.toCompletableFuture().join();
                assertEquals(
                        owner.owner.leaseGeneration(),
                        host.sessionRoute().sessionOwnerLeaseGeneration());
                host.assertLeases(f.owner.leaseGeneration(), owner.owner.leaseGeneration());
                host.assertIdentityAndSequence();
                owner.expireAndClaim();
                owner.publish(1);
                var nextDescriptors =
                        owner.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                var nextEvidence =
                        owner.repository.readOwnerLease("owner").toCompletableFuture().join();
                var nextStarted = new CompletableFuture<Void>();
                var nextHeld = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                var nextRepository =
                        intercept(
                                owner.repository,
                                method -> {
                                    if (!method.getName().equals("readOwnerLease")) return null;
                                    nextStarted.complete(null);
                                    return nextHeld;
                                });
                var nextPolling = host.pollSessionOwnerAsync(nextDescriptors, nextRepository);
                nextStarted.orTimeout(5, TimeUnit.SECONDS).join();
                var replacement = host.replaceActorBinding();
                nextHeld.complete(nextEvidence);
                nextPolling.toCompletableFuture().join();
                assertEquals(replacement, host.sessionRoute());
            }
        }
    }

    @RepeatedTest(100)
    void onePendingAuthorityDoesNotHoldAnotherBindingRefresh() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            var otherBefore = f.actor("other", "entry", 1);
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                var other = host.addRemoteActor("other", otherBefore);
                f.expireAndClaim();
                f.publish(1);
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor("actor"), before));
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor("other"), otherBefore));
                var firstRead = new java.util.concurrent.atomic.AtomicReference<String>();
                var held = new CompletableFuture<ZLinkAuthorityReadResult>();
                var published = new CompletableFuture<String>();
                host.sessionMeshNode()
                        .setSessionActorFenceHandler(
                                (node, socket, session, actor, binding, lease, publish) -> {
                                    if (publish.getAsBoolean()) published.complete(actor.actorId());
                                    return CompletableFuture.completedFuture(
                                            OptionalLong.of(host.actorLease(actor)));
                                });
                var repository =
                        (ZLinkLocationRepository)
                                Proxy.newProxyInstance(
                                        ZLinkLocationRepository.class.getClassLoader(),
                                        new Class<?>[] {ZLinkLocationRepository.class},
                                        (proxy, method, args) -> {
                                            if (method.getName().equals("read")
                                                    && firstRead.compareAndSet(
                                                            null, (String) args[0])) return held;
                                            try {
                                                return method.invoke(f.repository, args);
                                            } catch (InvocationTargetException failure) {
                                                throw failure.getCause();
                                            }
                                        });
                var descriptors =
                        f.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                var polling = host.pollAsync(descriptors, repository);
                var refreshed = published.orTimeout(5, TimeUnit.SECONDS).join();
                assertEquals(
                        f.owner.leaseGeneration(),
                        refreshed.equals("actor") ? host.actorLease() : host.actorLease(other));
                held.complete(f.read(firstRead.get()));
                polling.toCompletableFuture().join();
                assertEquals(f.owner.leaseGeneration(), host.actorLease());
                assertEquals(f.owner.leaseGeneration(), host.actorLease(other));
                var actorCurrent = f.read(ZLinkAuthorityKeyCodec.actor("actor"));
                var otherCurrent = f.read(ZLinkAuthorityKeyCodec.actor("other"));
                f.expireAndClaim();
                f.publish(1);
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor("actor"), actorCurrent));
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor("other"), otherCurrent));
                var failedRead = new java.util.concurrent.atomic.AtomicReference<String>();
                var independent = new CompletableFuture<String>();
                host.sessionMeshNode()
                        .setSessionActorFenceHandler(
                                (node, socket, session, target, binding, lease, publish) -> {
                                    if (publish.getAsBoolean())
                                        independent.complete(target.actorId());
                                    return CompletableFuture.completedFuture(
                                            OptionalLong.of(host.actorLease(target)));
                                });
                var faulty =
                        (ZLinkLocationRepository)
                                Proxy.newProxyInstance(
                                        ZLinkLocationRepository.class.getClassLoader(),
                                        new Class<?>[] {ZLinkLocationRepository.class},
                                        (proxy, method, args) -> {
                                            if (method.getName().equals("read")
                                                    && failedRead.compareAndSet(
                                                            null, (String) args[0]))
                                                throw new IllegalStateException(
                                                        "injected authority read failure");
                                            try {
                                                return method.invoke(f.repository, args);
                                            } catch (InvocationTargetException failure) {
                                                throw failure.getCause();
                                            }
                                        });
                var next =
                        f.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                var failedPolling = host.pollAsync(next, faulty);
                var advanced = independent.orTimeout(5, TimeUnit.SECONDS).join();
                assertEquals(
                        f.owner.leaseGeneration(),
                        advanced.equals("actor") ? host.actorLease() : host.actorLease(other));
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () -> failedPolling.toCompletableFuture().join());
                assertEquals("injected authority read failure", failure.getCause().getMessage());
                host.poll(next, f.repository);
                assertEquals(f.owner.leaseGeneration(), host.actorLease());
                assertEquals(f.owner.leaseGeneration(), host.actorLease(other));
            }
        }
    }

    @RepeatedTest(100)
    void pendingSessionLeaseHoldsControlsAndExpiredEvidenceCannotRefreshTheGate() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            var owner =
                    new ZLinkP4RuntimeRecoveryTest.Fixture(
                            provider, RoutingId.from("session-node"));
            owner.expireAndClaim();
            owner.publish(1);
            var descriptors =
                    owner.repository
                            .listMeshNodes("game", ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items();
            var evidence = owner.repository.readOwnerLease("owner").toCompletableFuture().join();
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                var started = new CompletableFuture<Void>();
                var held = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                var repository =
                        intercept(
                                owner.repository,
                                method -> {
                                    if (!method.getName().equals("readOwnerLease")) return null;
                                    started.complete(null);
                                    return held;
                                });
                var polling = host.pollSessionOwnerAsync(descriptors, repository);
                started.orTimeout(5, TimeUnit.SECONDS).join();
                assertEquals(1, host.sessionRoute().sessionOwnerLeaseGeneration());
                assertEquals(7, host.sessionRoute().bindingGeneration());
                host.assertControlHeld();
                host.assertReplacementHeld();
                host.advanceNanos(java.time.Duration.ofHours(2).toNanos());
                held.complete(evidence);
                polling.toCompletableFuture().join();
                assertTrue(host.actorBindingPresent());
                assertEquals(1, host.sessionRoute().sessionOwnerLeaseGeneration());
                assertEquals(7, host.sessionRoute().bindingGeneration());
                host.assertControlHeld();
                host.assertReplacementHeld();
            }
        }
    }

    @RepeatedTest(100)
    void delayedAuthorityConfirmationCannotRefreshAReplacedBinding() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                f.expireAndClaim();
                f.publish(1);
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor("actor"), before));
                var after = f.read(ZLinkAuthorityKeyCodec.actor("actor"));
                var started = new CompletableFuture<Void>();
                var held = new CompletableFuture<ZLinkAuthorityReadResult>();
                var repository =
                        intercept(
                                f.repository,
                                method -> {
                                    if (!method.getName().equals("read")) return null;
                                    started.complete(null);
                                    return held;
                                });
                var descriptors =
                        f.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                var polling = host.pollAsync(descriptors, repository);
                started.orTimeout(5, TimeUnit.SECONDS).join();
                host.replaceStreamBinding();
                held.complete(after);
                polling.toCompletableFuture().join();
                assertEquals(before.ownerLeaseGeneration(), host.actorLease());
                assertTrue(host.streamRoutePresent());
                host.poll(descriptors, f.repository);
                assertEquals(f.owner.leaseGeneration(), host.actorLease());
            }
        }
    }

    @RepeatedTest(100)
    void authorityIdentityChangesInvalidateRouteAndInvalidLeaseKeepsCopies() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            f.expireAndClaim();
            f.publish(1);
            assertInstanceOf(
                    ZLinkAuthorityStored.class,
                    f.preserve(ZLinkAuthorityKeyCodec.actor("actor"), before));
            var after = f.read(ZLinkAuthorityKeyCodec.actor("actor"));
            var descriptors =
                    f.repository
                            .listMeshNodes("game", ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items();
            for (int mismatch = 0; mismatch < 5; mismatch++) {
                var allocation = after.allocation();
                var changedAllocation =
                        new ZLinkPlacementAllocation(
                                allocation.state(),
                                allocation.objectKind(),
                                allocation.stableType(),
                                mismatch == 3
                                        ? new ZLinkMeshNodeDescriptorKey(
                                                "game", RoutingId.from("other"))
                                        : allocation.descriptor(),
                                mismatch == 4 ? -1 : allocation.descriptorLifecycleGeneration(),
                                allocation.capacityBundle());
                var changed =
                        new ZLinkAuthoritySnapshot(
                                after.storeVersion(),
                                after.payload(),
                                mismatch == 0
                                        ? after.objectGeneration() + 1
                                        : after.objectGeneration(),
                                mismatch == 1
                                        ? after.authorityOwnerGeneration() + 1
                                        : after.authorityOwnerGeneration(),
                                mismatch == 2 ? "other" : after.ownerId(),
                                after.ownerLeaseGeneration(),
                                changedAllocation,
                                after.storeNow());
                try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                    var repository =
                            intercept(
                                    f.repository,
                                    method ->
                                            method.getName().equals("read")
                                                    ? CompletableFuture.completedFuture(changed)
                                                    : null);
                    host.poll(descriptors, repository);
                    assertEquals(before.ownerLeaseGeneration(), host.actorLease());
                    assertFalse(host.streamRoutePresent());
                    assertFalse(host.controlAllowed());
                }
            }
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                var repository =
                        intercept(
                                f.repository,
                                method ->
                                        method.getName().equals("readOwnerLease")
                                                ? CompletableFuture.completedFuture(
                                                        new ZLinkOwnerLeaseFound(
                                                                new ZLinkLocationOwnerToken(
                                                                        "owner",
                                                                        f.owner.leaseGeneration()
                                                                                + 1),
                                                                f.clock.now.plusSeconds(1),
                                                                f.clock.now))
                                                : null);
                host.poll(descriptors, repository);
                assertEquals(before.ownerLeaseGeneration(), host.actorLease());
                assertTrue(host.streamRoutePresent());
                assertFalse(host.controlAllowed());
            }
        }
    }

    @RepeatedTest(100)
    void actorOwnerRefreshesSessionFenceAndRejectsAnotherLifecycle() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                var owner =
                        new ZLinkP4RuntimeRecoveryTest.Fixture(
                                provider, RoutingId.from("session-node"));
                owner.expireAndClaim();
                owner.publish(1);
                var descriptors =
                        owner.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                host.pollSessionOwner(descriptors, owner.repository);
                assertTrue(host.actorBindingPresent(), "Session binding removed: " + descriptors);
                var route = host.sessionRoute();
                assertEquals(7, route.bindingGeneration());
                assertEquals(RoutingId.from("session"), route.sessionRid());
                assertEquals(owner.owner.leaseGeneration(), route.sessionOwnerLeaseGeneration());
                host.assertIdentityAndSequence();
                host.pollSessionOwnerRows(
                        List.of(
                                new ZLinkAutoConnectPeer(
                                        ZLinkAutoConnectType.ROUTE_MESH,
                                        "game",
                                        RoutingId.from("session-node"),
                                        ZLinkLocationRole.ROUTER,
                                        "inproc://session-node",
                                        1,
                                        false,
                                        -1,
                                        Map.of(),
                                        List.of(),
                                        "owner",
                                        owner.owner.leaseGeneration(),
                                        owner.clock.now)),
                        owner.repository);
                assertFalse(host.actorBindingPresent());
            }
        }
    }

    @RepeatedTest(100)
    void actorRecoveryRefreshesItsBindingWithoutRebind() throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            var before = f.actor("actor", "entry", 1);
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                host.assertActorRecoveryKeepsBindingAndSequence(before.ownerLeaseGeneration() + 1);
            }
        }
    }

    @RepeatedTest(100)
    void descriptorBeforePreserveKeepsCopiesThenPollingRefreshesOnlyExactIncarnation()
            throws Exception {
        for (boolean provider : new boolean[] {false, true}) {
            var f = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
            String id = "actor";
            var before = f.actor(id, "entry", 1);
            try (var host = new ZLinkSessionFenceTestHost(f.node.rid(), before)) {
                f.expireAndClaim();
                f.publish(1);
                var descriptors =
                        f.repository
                                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items();
                var rows = ZLinkSessionFenceTestHost.rows(descriptors);
                var discovery =
                        new ZLinkAutoConnectReconciler(
                                new ZLinkAutoConnectPlanner.Local(
                                        ZLinkAutoConnectType.ROUTE_MESH,
                                        "game",
                                        ZLinkLocationRole.ROUTER,
                                        RoutingId.from("session-node"),
                                        "inproc://session-node"),
                                (type, mesh, role) -> CompletableFuture.completedFuture(rows),
                                new ZLinkLocationAutoConnectHost.MeshNodeExecutor(
                                        host.sessionMeshNode(),
                                        Set.of(descriptors.getFirst().endpoint()),
                                        Map.of(),
                                        f.repository),
                                new ZLinkLocationOptions(),
                                () -> 0L,
                                failure -> {
                                    throw new AssertionError(failure);
                                });
                discovery.tick().toCompletableFuture().join();
                assertEquals(before.ownerLeaseGeneration(), host.actorLease());
                assertFalse(host.controlAllowed());
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        f.preserve(ZLinkAuthorityKeyCodec.actor(id), before));
                discovery.tick().toCompletableFuture().join();
                assertEquals(f.owner.leaseGeneration(), host.actorLease());
                assertTrue(host.controlAllowed());
                host.assertIdentityAndSequence();
            }
        }
    }
}
