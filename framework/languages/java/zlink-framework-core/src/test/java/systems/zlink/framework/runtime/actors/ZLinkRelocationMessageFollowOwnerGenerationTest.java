package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.testing.ZLinkDescriptorLeaseTestFixture;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;

final class ZLinkRelocationMessageFollowOwnerGenerationTest {
    private static final String ACTOR_ID = "follow-actor";
    private static final long OBJECT_GENERATION = 7;

    @Test
    void twoHopsAdoptTheStoreIssuedTenureRatherThanTheStagingGeneration() throws Exception {
        var routeA = route("node-a", 11, 3, 1);
        var routeB = route("node-b", 12, 10, 2);
        var routeC = route("node-c", 13, 29, 3);
        var forwarded = new ArrayList<ZLinkServiceM6BWireCodec.ActorRouteFence>();
        var first = runtime(routeB, forwarded);
        var second = runtime(routeC, forwarded);
        try {
            first.stageRelocationMessageFollow(routeA, route("node-b", 12, 3, 2));
            second.stageRelocationMessageFollow(routeB, route("node-c", 13, 10, 3));
            confirm(first, routeA);
            confirm(second, routeB);
            // Each hop forwards to the Store-issued tenure, not the staging
            // generation, so the second hop's source fence matches the first.
            var header =
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                            false, 0, null, 1, 42, 0, null, routeA);
            var failures = new ArrayList<Throwable>();
            assertTrue(
                    first.relayMessageFollow(
                            RoutingId.from("client"),
                            17,
                            header,
                            new byte[0],
                            List.of(Message.from("header"), Message.from("payload")),
                            null,
                            null,
                            failures::add,
                            () -> {}));
            assertTrue(failures.isEmpty(), failures.toString());
            assertEquals(List.of(routeB), forwarded);
            var secondHeader =
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                            false,
                            0,
                            null,
                            header.operationHigh(),
                            header.operationLow(),
                            header.messageFollowHopCount() + 1,
                            null,
                            forwarded.getFirst());
            assertTrue(
                    second.relayMessageFollow(
                            RoutingId.from("client"),
                            17,
                            secondHeader,
                            new byte[0],
                            List.of(Message.from("header"), Message.from("payload")),
                            null,
                            null,
                            failures::add,
                            () -> {}));
            assertTrue(failures.isEmpty(), failures.toString());
            assertEquals(List.of(routeB, routeC), forwarded);
        } finally {
            first.close();
            second.close();
        }
    }

    @Test
    void aDifferentTargetCannotReplaceTheStagedMessageFollowDestination() throws Exception {
        var source = route("node-a", 11, 3, 1);
        var staged = route("node-b", 12, 3, 2);
        var forwarded = new ArrayList<ZLinkServiceM6BWireCodec.ActorRouteFence>();
        var runtime = runtime(route("node-c", 13, 29, 3), forwarded);
        try {
            runtime.stageRelocationMessageFollow(source, staged);
            assertThrows(
                    java.util.concurrent.CompletionException.class, () -> confirm(runtime, source));
            // The refused confirmation leaves the staged destination in
            // place; a relayed message never reaches the other target.
            var failures = new ArrayList<Throwable>();
            runtime.relayMessageFollow(
                    RoutingId.from("client"),
                    17,
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                            false, 0, null, 1, 43, 0, null, source),
                    new byte[0],
                    List.of(Message.from("header"), Message.from("payload")),
                    null,
                    null,
                    failures::add,
                    () -> {});
            assertTrue(failures.isEmpty(), failures.toString());
            assertEquals(List.of(staged), forwarded);
        } finally {
            runtime.close();
        }
    }

    @Test
    void stagedTenureIsEligibleBeforeCommitAndAbsentTenureKeepsCallerOwnership() {
        var source = route("node-a", 11, 3, 1);
        var target = route("node-b", 12, 4, 2);
        var forwarded = new ArrayList<ZLinkServiceM6BWireCodec.ActorRouteFence>();
        var runtime = runtime(target, forwarded);
        try {
            var failures = new ArrayList<Throwable>();
            var released = new java.util.concurrent.atomic.AtomicInteger();
            var header =
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                            false, 0, null, 1, 43, 0, null, source);
            var ordinary = List.of(Message.from("header"), Message.from("payload"));
            assertFalse(
                    runtime.relayMessageFollow(
                            RoutingId.from("client"),
                            17,
                            header,
                            new byte[0],
                            ordinary,
                            null,
                            null,
                            failures::add,
                            released::incrementAndGet));
            assertEquals(0, released.get());
            assertEquals(7, ordinary.get(1).size());
            ordinary.forEach(Message::close);

            runtime.stageRelocationMessageFollow(source, target);
            assertTrue(
                    runtime.relayMessageFollow(
                            RoutingId.from("client"),
                            17,
                            header,
                            new byte[0],
                            List.of(Message.from("header"), Message.from("payload")),
                            null,
                            null,
                            failures::add,
                            released::incrementAndGet));
            assertEquals(List.of(target), forwarded);
            assertEquals(1, released.get());
            assertTrue(failures.isEmpty(), failures.toString());
        } finally {
            runtime.close();
        }
    }

    private static void confirm(
            ZLinkActorRuntime runtime, ZLinkServiceM6BWireCodec.ActorRouteFence source)
            throws Exception {
        runtime.confirmRelocationMessageFollow(source).toCompletableFuture().join();
    }

    private static ZLinkActorRuntime runtime(ZLinkServiceM6BWireCodec.ActorRouteFence issued) {
        return runtime(issued, new ArrayList<>());
    }

    private static ZLinkActorRuntime runtime(
            ZLinkServiceM6BWireCodec.ActorRouteFence issued,
            List<ZLinkServiceM6BWireCodec.ActorRouteFence> forwarded) {
        var node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "routingId" -> RoutingId.from("local");
                                            case "forwardMessageFollowActor" -> {
                                                forwarded.add(
                                                        (ZLinkServiceM6BWireCodec.ActorRouteFence)
                                                                arguments[1]);
                                                yield CompletableFuture.completedFuture(List.of());
                                            }
                                            default -> null;
                                        });
        var now = Instant.now();
        var snapshot =
                new ZLinkAuthoritySnapshot(
                        "issued-version",
                        new byte[0],
                        OBJECT_GENERATION,
                        issued.authorityOwnerGeneration(),
                        "issued-owner",
                        issued.ownerLeaseGeneration(),
                        new ZLinkPlacementAllocation(
                                ZLinkPlacementAllocationState.ACTIVE,
                                ZLinkPlacementObjectKind.ACTOR,
                                "actor-type",
                                new ZLinkMeshNodeDescriptorKey("mesh", issued.actor().nodeRid()),
                                issued.targetNodeGeneration(),
                                ZLinkPlacementCapacityBundle.actor(1)),
                        now);
        var store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" ->
                                                    CompletableFuture.completedFuture(snapshot);
                                            case "readOwnerLease" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkOwnerLeaseFound(
                                                                    new ZLinkLocationOwnerToken(
                                                                            "issued-owner",
                                                                            issued
                                                                                    .ownerLeaseGeneration()),
                                                                    now.plusSeconds(30),
                                                                    now));
                                            default -> null;
                                        });
        var runtime =
                new ZLinkActorRuntime(
                        node,
                        Map.<String, Class<? extends ZLinkActorFactory>>of(),
                        Duration.ofSeconds(1),
                        new ZLinkJsonMessageSerializer());
        runtime.setStoreLocationResolvers(ZLinkDescriptorLeaseTestFixture.resolver(store));
        return runtime;
    }

    private static ZLinkServiceM6BWireCodec.ActorRouteFence route(
            String node, long lifecycle, long authority, long lease) {
        return new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(RoutingId.from(node), ACTOR_ID, OBJECT_GENERATION),
                lifecycle,
                authority,
                lease);
    }
}
