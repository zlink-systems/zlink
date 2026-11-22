package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.*;
import systems.zlink.framework.runtime.internal.locations.*;

import java.lang.reflect.Proxy;
import java.time.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

final class ZLinkPlacementDiscoveryTest {
    @RepeatedTest(40)
    void placementRefreshesOnlyAnObservationWithoutEligibleCandidates() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.state = ZLinkFrameworkRuntimeState.DRAINING;
                fixture.refresh();
                fixture.rid = RoutingId.from("replacement");
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.weight = 100;
                assertEquals(fixture.rid, fixture.select(1000).join().rid());
                assertEquals(2, fixture.lists.get());
                assertEquals(fixture.rid, fixture.select(1000).join().rid());
                assertEquals(2, fixture.lists.get());
            }
        }
        try (var fixture = new Fixture(ZLinkPlacementObjectKind.INSTANCE_SPOT)) {
            fixture.state = ZLinkFrameworkRuntimeState.DRAINING;
            fixture.refresh();
            fixture.rid = RoutingId.from("replacement");
            fixture.state = ZLinkFrameworkRuntimeState.SERVING;
            for (String type : Arrays.asList("type", null)) {
                var selected =
                        fixture.resolver
                                .selectInstancePlacement(
                                        "mesh",
                                        type,
                                        Set.of(fixture.rid),
                                        System.currentTimeMillis() + 1000)
                                .toCompletableFuture()
                                .join();
                assertEquals(fixture.rid, selected.descriptor().rid());
                assertEquals("type", selected.stableType());
                assertEquals(2, fixture.lists.get());
            }
        }
    }

    @RepeatedTest(40)
    void placementRefreshUsesTheRequestDeadline() {
        try (var fixture = new Fixture(ZLinkPlacementObjectKind.USER_SPOT)) {
            fixture.deleted = true;
            fixture.refresh();
            var page = new CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>();
            fixture.heldPage = page;
            var selected = fixture.select(25);
            assertFailure(selected, ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
            assertEquals(2, fixture.lists.get());

            fixture.state = ZLinkFrameworkRuntimeState.SERVING;
            page.complete(new ZLinkLocationPage<>(List.of(fixture.descriptor()), null));
            assertFailure(fixture.select(1000), ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(3, fixture.lists.get());
        }
    }

    @RepeatedTest(40)
    void unobservedMeshWaitsForFirstDiscoveryPublication() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            for (boolean localPublication : List.of(false, true)) {
                try (var fixture = new Fixture(kind)) {
                    var selected = fixture.select(1000);
                    assertFalse(
                            selected.isDone(), "unobserved mesh must wait for first publication");
                    assertEquals(0, fixture.lists.get());
                    assertEquals(0, fixture.leaseReads.get());
                    fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                    if (localPublication) {
                        fixture.resolver
                                .observePublishedMeshNode(fixture.descriptor())
                                .toCompletableFuture()
                                .join();
                    } else fixture.refresh();
                    assertEquals(fixture.rid, selected.join().rid());
                    assertEquals(localPublication ? 0 : 1, fixture.lists.get());
                    assertEquals(1, fixture.leaseReads.get());
                }
            }
        }
    }

    @RepeatedTest(40)
    void firstScanAlreadyInFlightPublishesToWaitingRequest() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                var page = new CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>();
                fixture.heldPage = page;
                var scan =
                        fixture.resolver.listPeers(ZLinkAutoConnectType.ROUTE_MESH, "mesh", null);
                var selected = fixture.select(1000);
                assertFalse(selected.isDone());
                page.complete(new ZLinkLocationPage<>(List.of(fixture.descriptor()), null));
                scan.toCompletableFuture().join();
                assertEquals(fixture.rid, selected.join().rid());
                assertEquals(1, fixture.lists.get());
            }
        }
    }

    @RepeatedTest(40)
    void firstEmptyObservationIsImmediatelyUnavailable() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.deleted = true;
                fixture.refresh();
                var selected = fixture.select(1000);
                assertTrue(selected.isDone(), "observed empty snapshot must not wait");
                assertFailure(selected, ZLinkFrameworkErrorKind.UNAVAILABLE);
                assertEquals(2, fixture.lists.get());
                assertEquals(0, fixture.leaseReads.get());
            }
        }
    }

    @RepeatedTest(40)
    void unobservedMeshPreservesDeadlineAndCloseCancellation() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                assertFailure(fixture.select(20), ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
                assertEquals(0, fixture.lists.get());
                assertEquals(0, fixture.leaseReads.get());
                var waiting = fixture.select(1000);
                assertFalse(waiting.isDone());
                fixture.resolver.close();
                assertTrue(waiting.isCompletedExceptionally());
            }
        }
    }

    @RepeatedTest(40)
    void unavailableSourceStatusDoesNotThrowSynchronously() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.refresh();
                fixture.beforeStatus =
                        () -> {
                            throw new IllegalStateException("source closing");
                        };
                var selected = assertDoesNotThrow(() -> fixture.select(1000));
                assertFalse(selected.isDone());
                fixture.refresh();
                assertEquals(fixture.rid, selected.join().rid());
            }
        }
    }

    @RepeatedTest(40)
    void sharedDiscoveryWaitsWithoutRequestReadsWhileTargetsArePreparing() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            for (String transition : List.of("serving", "deleted", "zero", "full", "lease")) {
                try (var fixture = new Fixture(kind)) {
                    fixture.refresh();
                    var requests = new ArrayList<CompletableFuture<ZLinkMeshNodeDescriptor>>();
                    for (int i = 0; i < 13; i++) requests.add(fixture.select(1000));
                    requests.forEach(
                            request -> assertFalse(request.isDone(), "Preparing must wait"));
                    assertEquals(1, fixture.lists.get());
                    assertEquals(1, fixture.leaseReads.get());
                    switch (transition) {
                        case "serving" -> fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                        case "deleted" -> fixture.deleted = true;
                        case "zero" -> fixture.weight = 0;
                        case "full" -> fixture.full = true;
                        case "lease" -> fixture.live = false;
                    }
                    fixture.refresh();
                    requests.forEach(
                            request -> {
                                assertTrue(
                                        request.isDone(),
                                        "refresh must decide immediately: " + transition);
                                if (transition.equals("serving"))
                                    assertEquals(fixture.rid, request.join().rid());
                                else assertFailure(request, ZLinkFrameworkErrorKind.UNAVAILABLE);
                            });
                    assertEquals(
                            transition.equals("serving") ? 2 : 2 + requests.size(),
                            fixture.lists.get());
                    assertEquals(
                            transition.equals("deleted")
                                    ? 1
                                    : transition.equals("lease") ? 2 + requests.size() : 2,
                            fixture.leaseReads.get());
                }
            }
        }
    }

    @RepeatedTest(40)
    void emptyZeroUnknownAndFullAreTerminalWhilePreparingDeadlineIsPreserved() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            for (String input :
                    List.of(
                            "empty",
                            "zero",
                            "unknown",
                            "full",
                            "excluded",
                            "expired",
                            "deadline")) {
                try (var fixture = new Fixture(kind)) {
                    switch (input) {
                        case "empty" -> fixture.deleted = true;
                        case "zero" -> fixture.weight = 0;
                        case "full" -> fixture.full = true;
                    }
                    fixture.refresh();
                    if (input.equals("expired")) {
                        fixture.live = false;
                        fixture.nanos.addAndGet(Duration.ofSeconds(31).toNanos());
                    }
                    long deadline =
                            System.currentTimeMillis() + (input.equals("deadline") ? 50 : 1000);
                    var selected =
                            fixture.resolver
                                    .selectPlacement(
                                            "mesh",
                                            kind,
                                            input.equals("unknown") ? "unknown" : "type",
                                            kind == ZLinkPlacementObjectKind.ACTOR,
                                            fixture.source,
                                            input.equals("excluded")
                                                    ? Set.of(
                                                            new ZLinkMeshNodeDescriptorKey(
                                                                    "mesh", fixture.rid))
                                                    : Set.of(),
                                            deadline)
                                    .toCompletableFuture();
                    if (!input.equals("deadline")) assertTrue(selected.isDone(), input);
                    assertFailure(
                            selected,
                            input.equals("deadline")
                                    ? ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED
                                    : ZLinkFrameworkErrorKind.UNAVAILABLE);
                    assertEquals(input.equals("deadline") ? 1 : 2, fixture.lists.get());
                    assertEquals(
                            input.equals("empty") ? 0 : input.equals("expired") ? 2 : 1,
                            fixture.leaseReads.get());
                }
            }
        }
    }

    @RepeatedTest(40)
    void refreshSubscriptionBoundaryCancellationAndCloseDoNotLoseOrReviveRequests() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.refresh();
                var first = fixture.select(1000);
                var cancelled = fixture.select(1000);
                cancelled.cancel(false);
                var second = fixture.select(1000);
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.refresh();
                assertEquals(fixture.rid, first.join().rid());
                assertEquals(fixture.rid, second.join().rid());
                assertTrue(cancelled.isCancelled());
                assertEquals(fixture.rid, fixture.select(1000).join().rid());
                fixture.state = ZLinkFrameworkRuntimeState.PREPARING;
                fixture.refresh();
                var waiting = fixture.select(1000);
                fixture.resolver.close();
                assertTrue(waiting.isCompletedExceptionally());
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.refresh();
                assertTrue(fixture.select(1000).isCompletedExceptionally());
            }
            try (var fixture = new Fixture(kind)) {
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.refresh();
                fixture.beforeStatus =
                        () -> {
                            fixture.deleted = true;
                            fixture.refresh();
                        };
                assertFailure(fixture.select(1000), ZLinkFrameworkErrorKind.UNAVAILABLE);
                assertEquals(3, fixture.lists.get());
            }
        }
    }

    @RepeatedTest(40)
    void confirmedLocalPublicationFencesOlderDiscoveryAndPublicationResults() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.refresh();
                var pending = fixture.select(1000);
                var old = fixture.descriptor();
                var heldPage = new CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>();
                fixture.heldPage = heldPage;
                var read =
                        fixture.resolver
                                .listPeers(ZLinkAutoConnectType.ROUTE_MESH, "mesh", null)
                                .toCompletableFuture();
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.revision = 2;
                fixture.resolver
                        .observePublishedMeshNode(fixture.descriptor())
                        .toCompletableFuture()
                        .join();
                assertEquals(fixture.rid, pending.join().rid());
                fixture.resolver.observePublishedMeshNode(old).toCompletableFuture().join();
                heldPage.complete(new ZLinkLocationPage<>(List.of(old), null));
                read.join();
                assertEquals(fixture.rid, fixture.select(1000).join().rid());
                fixture.weight = 0;
                fixture.revision = 3;
                fixture.resolver
                        .observePublishedMeshNode(fixture.descriptor())
                        .toCompletableFuture()
                        .join();
                assertFailure(fixture.select(1000), ZLinkFrameworkErrorKind.UNAVAILABLE);
                assertEquals(3, fixture.lists.get());
            }
        }
    }

    @RepeatedTest(40)
    void exhaustedCapacityCanBecomeAvailableOnTheStoreRefresh() {
        for (var kind :
                List.of(ZLinkPlacementObjectKind.ACTOR, ZLinkPlacementObjectKind.USER_SPOT)) {
            try (var fixture = new Fixture(kind)) {
                fixture.state = ZLinkFrameworkRuntimeState.SERVING;
                fixture.full = true;
                fixture.refresh();
                fixture.full = false;
                assertEquals(fixture.rid, fixture.select(1000).join().rid());
                assertEquals(2, fixture.lists.get());
                assertEquals(fixture.rid, fixture.select(1000).join().rid());
                assertEquals(2, fixture.lists.get());
            }
        }
    }

    @RepeatedTest(40)
    void existingActorEntryRouteDoesNotConsumeNewActorCapacity() {
        try (var fixture = new Fixture(ZLinkPlacementObjectKind.ACTOR)) {
            fixture.state = ZLinkFrameworkRuntimeState.SERVING;
            fixture.full = true;
            fixture.refresh();
            var existing =
                    fixture.resolver
                            .selectPlacement(
                                    "mesh",
                                    ZLinkPlacementObjectKind.ACTOR,
                                    "type",
                                    false,
                                    fixture.source,
                                    Set.of(),
                                    System.currentTimeMillis() + 1000)
                            .toCompletableFuture();
            assertEquals(fixture.rid, existing.join().rid());
            assertFailure(fixture.select(1000), ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(2, fixture.lists.get());
        }
    }

    private static void assertFailure(CompletableFuture<?> request, ZLinkFrameworkErrorKind kind) {
        var error = assertThrows(CompletionException.class, request::join);
        assertEquals(
                kind, assertInstanceOf(ZLinkFrameworkException.class, error.getCause()).kind());
    }

    private static final class Fixture implements AutoCloseable {
        final ZLinkPlacementObjectKind kind;
        RoutingId rid = RoutingId.from("local");
        final AtomicInteger lists = new AtomicInteger();
        final AtomicInteger leaseReads = new AtomicInteger();
        final AtomicLong nanos = new AtomicLong();
        ZLinkFrameworkRuntimeState state = ZLinkFrameworkRuntimeState.PREPARING;
        int weight = 100;
        long revision = 1;
        CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> heldPage;
        boolean live = true;
        boolean full;
        boolean deleted;
        Runnable beforeStatus = () -> {};
        final ZLinkStoreLocationResolvers resolver;
        final ZLinkInternalMeshNode source;

        Fixture(ZLinkPlacementObjectKind kind) {
            this.kind = kind;
            var store =
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "listMeshNodes" -> {
                                                    lists.incrementAndGet();
                                                    if (heldPage != null) {
                                                        var page = heldPage;
                                                        heldPage = null;
                                                        yield page;
                                                    }
                                                    yield CompletableFuture.completedFuture(
                                                            new ZLinkLocationPage<>(
                                                                    deleted
                                                                            ? List.of()
                                                                            : List.of(descriptor()),
                                                                    null));
                                                }
                                                case "readOwnerLease" -> {
                                                    leaseReads.incrementAndGet();
                                                    yield CompletableFuture.completedFuture(
                                                            live
                                                                    ? new ZLinkOwnerLeaseFound(
                                                                            new ZLinkLocationOwnerToken(
                                                                                    "owner", 1),
                                                                            Instant.EPOCH
                                                                                    .plusSeconds(
                                                                                            30),
                                                                            Instant.EPOCH)
                                                                    : new ZLinkOwnerLeaseMissing());
                                                }
                                                default ->
                                                        throw new AssertionError(
                                                                "unexpected Store call "
                                                                        + method.getName());
                                            });
            resolver =
                    new ZLinkStoreLocationResolvers(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            new ZLinkLiveLocationRows(
                                    new ZLinkOwnerLeaseTracker(
                                            store, Duration.ofNanos(1), nanos::get),
                                    Duration.ofSeconds(1)));
            source =
                    (ZLinkInternalMeshNode)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkInternalMeshNode.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "status" -> {
                                                    var action = beforeStatus;
                                                    beforeStatus = () -> {};
                                                    action.run();
                                                    yield new MeshNodeStatus(
                                                            MeshNodeState.READY,
                                                            rid,
                                                            "mesh",
                                                            "tcp://127.0.0.1:1",
                                                            1,
                                                            1,
                                                            0,
                                                            0,
                                                            0,
                                                            0,
                                                            0,
                                                            0,
                                                            0,
                                                            0,
                                                            0);
                                                }
                                                case "peers" -> List.of();
                                                default ->
                                                        throw new AssertionError(
                                                                "unexpected mesh call "
                                                                        + method.getName());
                                            });
        }

        ZLinkMeshNodeDescriptor descriptor() {
            var usage = new ZLinkCapacityUsage(full ? 1 : 0, 0, 1);
            return new ZLinkMeshNodeDescriptor(
                    "mesh",
                    rid,
                    1,
                    revision,
                    "tcp://127.0.0.1:1",
                    Map.of(),
                    1,
                    List.of(
                            new ZLinkObjectCapability(
                                    kind,
                                    "type",
                                    ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                    true,
                                    0,
                                    kind == ZLinkPlacementObjectKind.ACTOR)),
                    ZLinkMeshNodeObjectRole.SERVER,
                    Optional.of("entry"),
                    weight,
                    new ZLinkPlacementCapacity(
                            usage,
                            usage,
                            kind == ZLinkPlacementObjectKind.ACTOR
                                    ? List.of()
                                    : List.of(new ZLinkSpotTypeCapacity(kind, "type", usage))),
                    new ZLinkActivationConcurrency(0, 1),
                    Optional.empty(),
                    state,
                    "security",
                    "owner",
                    1,
                    Instant.EPOCH);
        }

        void refresh() {
            nanos.incrementAndGet();
            resolver.listPeers(ZLinkAutoConnectType.ROUTE_MESH, "mesh", null)
                    .toCompletableFuture()
                    .join();
        }

        CompletableFuture<ZLinkMeshNodeDescriptor> select(long timeout) {
            return resolver.selectPlacement(
                            "mesh",
                            kind,
                            "type",
                            kind == ZLinkPlacementObjectKind.ACTOR,
                            source,
                            Set.of(),
                            System.currentTimeMillis() + timeout)
                    .toCompletableFuture();
        }

        public void close() {
            resolver.close();
        }
    }
}
