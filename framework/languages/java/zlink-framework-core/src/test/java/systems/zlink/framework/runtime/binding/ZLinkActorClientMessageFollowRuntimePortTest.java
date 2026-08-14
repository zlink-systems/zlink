package systems.zlink.framework.runtime.binding;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.actors.ZLinkActorClientRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkActorClientMessageFollowRuntimePortTest {
    private static final String ACTOR_ID = "runtime-port-actor";
    private static final RoutingId SOURCE_RID =
        RoutingId.from("runtime-port-source");
    private static final RoutingId TARGET_RID =
        RoutingId.from("runtime-port-target");
    private static final RoutingId CLIENT_RID =
        RoutingId.from("runtime-port-client");

    @Test
    void targetJoinedSendFollowsExactCommittedSourceAndRefreshesTheNextCall()
        throws Exception {
        JsonNode scenario = trafficScenario(
            fixture("framework/runtime/conformance/relocation-behavior-v1.json"),
            "message-follow-delayed-notice-next-call");
        JsonNode afterFirst = checkpoint(
            scenario, "afterFirstReplyBeforeNotice");
        JsonNode afterNext = checkpoint(
            scenario, "afterImmediateNextOperation");
        JsonNode afterNotice = checkpoint(
            scenario, "afterRelayNotice");
        JsonNode afterFollowing = checkpoint(
            scenario, "afterFollowingOperation");
        String sourceEndpoint =
            "inproc://runtime-port-source-" + System.nanoTime();
        String targetEndpoint =
            "inproc://runtime-port-target-" + System.nanoTime();
        String clientEndpoint =
            "inproc://runtime-port-client-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh");
             var clientNode = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(SOURCE_RID);
            source.setBind(sourceEndpoint);
            source.setApplicationStreamCodecResolver(
                ignored -> Optional.of(ZLinkStreamCodec.JSON));
            target.setRoutingId(TARGET_RID);
            target.setBind(targetEndpoint);
            target.setApplicationStreamCodecResolver(
                ignored -> Optional.of(ZLinkStreamCodec.JSON));
            clientNode.setRoutingId(CLIENT_RID);
            clientNode.setBind(clientEndpoint);
            clientNode.setApplicationStreamCodecResolver(
                ignored -> Optional.of(ZLinkStreamCodec.JSON));
            source.setPeerAuthorityResolver(
                ZLinkActorClientMessageFollowRuntimePortTest::peerAuthority);
            target.setPeerAuthorityResolver(
                ZLinkActorClientMessageFollowRuntimePortTest::peerAuthority);
            clientNode.setPeerAuthorityResolver(
                ZLinkActorClientMessageFollowRuntimePortTest::peerAuthority);

            List<String> sourcePackets = new CopyOnWriteArrayList<>();
            List<String> targetPackets = new CopyOnWriteArrayList<>();
            recordActorPackets(source, sourcePackets);
            recordActorPackets(target, targetPackets);

            source.start();
            target.start();
            clientNode.start();
            source.connectPeer(clientEndpoint, CLIENT_RID);
            target.connectPeer(clientEndpoint, CLIENT_RID);
            clientNode.connectPeer(sourceEndpoint, SOURCE_RID);
            clientNode.connectPeer(targetEndpoint, TARGET_RID);
            awaitAdmitted(source, CLIENT_RID);
            awaitAdmitted(target, CLIENT_RID);
            awaitAdmitted(clientNode, SOURCE_RID);
            awaitAdmitted(clientNode, TARGET_RID);

            ZLinkBackendActorRef sourceActor;
            ZLinkBackendActorRef targetActor;
            try (Message create = Message.from("source")) {
                sourceActor = source.spotNode().createActor(
                    ACTOR_ID, 7, create);
            }
            try (Message create = Message.from("target")) {
                targetActor = target.spotNode().createActor(
                    ACTOR_ID, sourceActor.generation(), create);
            }
            source.spotNode().rememberActorAuthority(sourceActor, 11, 3);
            target.spotNode().rememberActorAuthority(targetActor, 12, 5);

            AtomicReference<ZLinkAuthoritySnapshot> authority =
                new AtomicReference<>(snapshot(
                    "v1", SOURCE_RID, source.lifecycleGeneration(),
                    sourceActor.generation(), 11, "source-owner", 3));
            AtomicInteger authorityReads = new AtomicInteger();
            ZLinkStoreLocationResolvers routes = new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(
                    authorityStore(authority, authorityReads)),
                locationOptions());
            ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
                clientNode::spotNode,
                routes,
                new ZLinkJsonMessageSerializer(),
                Duration.ofSeconds(2),
                (ignoredBackend, ignoredKey) -> (submission, cleanup) -> {
                    try {
                        return submission.get()
                            ? CompletableFuture.completedFuture(null)
                            : CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "Actor runtime port rejected the send"));
                    } finally {
                        cleanup.run();
                    }
                });

            client.sendToActor(ACTOR_ID, new PrimeRoute()).submit()
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            awaitPacket(sourcePackets, "PrimeRoute");
            assertEquals(1, authorityReads.get());

            authority.set(snapshot(
                "v2", TARGET_RID, target.lifecycleGeneration(),
                targetActor.generation(), 12, "target-owner", 5));
            var sourceRoute = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                sourceActor,
                source.lifecycleGeneration(),
                11,
                3);
            var targetRoute = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                targetActor,
                target.lifecycleGeneration(),
                12,
                5);
            var notice = new ZLinkServiceMessageFollowWireCodec.Notice(
                messageFollowRoute(sourceRoute),
                messageFollowRoute(targetRoute),
                1,
                1,
                1,
                101,
                103,
                0);
            AtomicInteger notices = new AtomicInteger();
            CompletableFuture<Boolean> invalidated = new CompletableFuture<>();
            CompletableFuture<Boolean> delayedNotice = new CompletableFuture<>();
            clientNode.setMessageFollowHandler((actualSource, actualNotice) -> {
                assertEquals(SOURCE_RID, actualSource);
                assertEquals(notice, actualNotice);
                assertEquals(SOURCE_RID,
                    actualNotice.source().targetNodeRid());
                assertEquals(TARGET_RID,
                    actualNotice.target().targetNodeRid());
                boolean removed = routes.invalidateRouteIfMatches(actualNotice);
                if (notices.incrementAndGet() == 1) {
                    invalidated.complete(removed);
                } else {
                    delayedNotice.complete(removed);
                }
            });

            AtomicInteger relays = new AtomicInteger();
            source.spotNode().setMessageFollowRelayHandler(
                (sourceNodeRid,
                    sourceNodeGeneration,
                    header,
                    acceptedJournalRecord,
                    parts,
                      contentType,

                      reply,
                      failure,
                      terminalRelease) -> {
                    if (!sourceRoute.equals(header.target())) {
                        return false;
                    }
                    assertEquals(CLIENT_RID, sourceNodeRid);
                    assertEquals(clientNode.lifecycleGeneration(),
                        sourceNodeGeneration);
                    relays.incrementAndGet();
                    try {
                        assertTrue(target.spotNode().sendToActor(
                            targetActor, parts, SendFlags.DONT_WAIT));
                      } finally {
                          parts.forEach(Message::close);
                          terminalRelease.run();
                      }
                    return true;
                });

            source.spotNode().closeActorBoundSession(
                sourceActor, Duration.ofSeconds(1));
            assertEquals(sourceActor,
                source.spotNode().actorLookup(ACTOR_ID));

            client.sendToActor(ACTOR_ID, new JoinedState()).submit()
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            awaitPacket(targetPackets, "JoinedState");

            assertEquals(1, relays.get());
            assertEquals(1, count(targetPackets, "JoinedState"));
            assertFalse(sourcePackets.contains("JoinedState"));
            assertEquals(1, authorityReads.get(),
                "the original operation must use the primed source route");
            assertCheckpoint(
                afterFirst,
                1,
                sourcePackets,
                targetPackets,
                "JoinedState");

            client.sendToActor(ACTOR_ID, new NextState()).submit()
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            awaitPacket(targetPackets, "NextState");
            assertEquals(1, count(targetPackets, "NextState"));
            assertEquals(2, relays.get(),
                "the immediate next call must use the still-cached source route");
            assertEquals(1, authorityReads.get());
            assertCheckpoint(
                afterNext,
                2,
                sourcePackets,
                targetPackets,
                "JoinedState",
                "NextState");

            source.sendMessageFollow(CLIENT_RID, notice)
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertTrue(invalidated.get(2, TimeUnit.SECONDS));
            assertEquals(
                afterNotice.path("targetHandlerCount").asInt(),
                applicationDeliveries(targetPackets,
                    "JoinedState", "NextState"));

            client.sendToActor(ACTOR_ID, new AfterNotice()).submit()
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            awaitPacket(targetPackets, "AfterNotice");
            assertEquals(1, count(targetPackets, "AfterNotice"));
            assertEquals(2, relays.get());
            assertEquals(2, authorityReads.get(),
                "the notice must refresh the next operation from Store");
            assertCheckpoint(
                afterFollowing,
                3,
                sourcePackets,
                targetPackets,
                "JoinedState",
                "NextState",
                "AfterNotice");

            source.sendMessageFollow(CLIENT_RID, notice)
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertFalse(delayedNotice.get(2, TimeUnit.SECONDS),
                "an old-source notice must preserve the cached target tenure");
            client.sendToActor(ACTOR_ID, new AfterDelayedNotice()).submit()
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            awaitPacket(targetPackets, "AfterDelayedNotice");
            assertEquals(1, count(targetPackets, "AfterDelayedNotice"));
            assertEquals(2, relays.get());
            assertEquals(2, authorityReads.get(),
                "the delayed duplicate must not evict the v2 route");
        }
    }

    private static CompletableFuture<Optional<
        ZLinkInternalMeshNode.PeerAuthorityFence>> peerAuthority(
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) {
        return CompletableFuture.completedFuture(Optional.of(
            new ZLinkInternalMeshNode.PeerAuthorityFence(
                nodeRid,
                nodeGeneration,
                "runtime-port-owner-" + nodeRid,
                1)));
    }

    private static void recordActorPackets(
        ZLinkJavaRawMeshNode node,
        List<String> packets) {
        node.spotNode().entrySpot().onDispatchEvent(info -> {
            if (info.event() != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                return;
            }
            try {
                for (int index = 0; index < info.actorMessages().size();) {
                    var first = info.actorMessages().get(index);
                    String packetName = ZLinkStreamHeaderCodec
                        .decodeOrPlain(first.message().data())
                        .packetName();
                    packets.add(packetName);
                    do {
                        index++;
                    } while (index < info.actorMessages().size()
                        && info.actorMessages().get(index - 1).hasMore());
                }
            } finally {
                info.actorMessages().forEach(received -> received.close());
            }
        });
    }

    private static ZLinkLocationOptions locationOptions() {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ofSeconds(10));
        return options;
    }

    private static ZLinkLocationRepository authorityStore(
        AtomicReference<ZLinkAuthoritySnapshot> authority,
        AtomicInteger reads) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "read" -> {
                    reads.incrementAndGet();
                    yield CompletableFuture.completedFuture(authority.get());
                }
                case "readOwnerLease" -> CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseFound(
                        new ZLinkLocationOwnerToken(
                            (String) arguments[0],
                            "source-owner".equals(arguments[0]) ? 3 : 5),
                        Instant.now().plusSeconds(60),
                        Instant.now()));
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkAuthoritySnapshot snapshot(
        String storeVersion,
        RoutingId nodeRid,
        long nodeGeneration,
        long objectGeneration,
        long authorityOwnerGeneration,
        String ownerId,
        long ownerLeaseGeneration) {
        byte[] payload = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "probe",
            ACTOR_ID,
            nodeRid + "-entry",
            1,
            1,
            ownerId,
            ownerLeaseGeneration,
            "mesh",
            nodeRid,
            nodeGeneration);
        return new ZLinkAuthoritySnapshot(
            storeVersion,
            payload,
            objectGeneration,
            authorityOwnerGeneration,
            ownerId,
            ownerLeaseGeneration,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "probe",
                new ZLinkMeshNodeDescriptorKey("mesh", nodeRid),
                nodeGeneration,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
    }

    private static ZLinkServiceMessageFollowWireCodec.ActorRoute
        messageFollowRoute(ZLinkServiceM6BWireCodec.ActorRouteFence route) {
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            route.actor().actorId(),
            route.actor().generation(),
            route.actor().nodeRid(),
            route.targetNodeGeneration(),
            route.authorityOwnerGeneration(),
            route.ownerLeaseGeneration());
    }

    private static void awaitPacket(List<String> packets, String expected)
        throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (!packets.contains(expected) && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(packets.contains(expected),
            "missing packet " + expected + " in " + new ArrayList<>(packets));
    }

    private static int count(List<String> packets, String expected) {
        return (int) packets.stream().filter(expected::equals).count();
    }

    private static void assertCheckpoint(
        JsonNode checkpoint,
        int expectedDeliveries,
        List<String> sourcePackets,
        List<String> targetPackets,
        String... applicationPackets) {
        assertEquals(expectedDeliveries,
            checkpoint.path("deliveryCount").asInt());
        assertEquals(
            checkpoint.path("sourceHandlerCount").asInt(),
            applicationDeliveries(sourcePackets, applicationPackets));
        assertEquals(
            checkpoint.path("targetHandlerCount").asInt(),
            applicationDeliveries(targetPackets, applicationPackets));
    }

    private static int applicationDeliveries(
        List<String> packets,
        String... applicationPackets) {
        int count = 0;
        for (String packet : applicationPackets) {
            count += count(packets, packet);
        }
        return count;
    }

    private static JsonNode trafficScenario(
        JsonNode fixture,
        String name) {
        for (JsonNode scenario : fixture.path("trafficScenarios")) {
            if (name.equals(scenario.path("name").asText())) {
                return scenario;
            }
        }
        throw new AssertionError(
            "shared relocation fixture has no traffic scenario " + name);
    }

    private static JsonNode checkpoint(JsonNode scenario, String name) {
        for (JsonNode checkpoint : scenario.path("checkpoints")) {
            if (name.equals(checkpoint.path("name").asText())) {
                return checkpoint;
            }
        }
        throw new AssertionError(
            "shared relocation fixture has no checkpoint " + name);
    }

    private static JsonNode fixture(String relativePath) throws Exception {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(relativePath);
            if (Files.isRegularFile(candidate)) {
                return new ObjectMapper().readTree(Files.readString(candidate));
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found: " + relativePath);
    }

    private static void awaitAdmitted(
        ZLinkJavaRawMeshNode node,
        RoutingId peerRid)
        throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream().noneMatch(peer ->
                peer.routingId().equals(peerRid)
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding.spot
                            .MeshPeerState.ADMITTED)
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(node.peers().stream().anyMatch(peer ->
            peer.routingId().equals(peerRid)
                && peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED));
    }

    private record PrimeRoute() {
    }

    private record JoinedState() {
    }

    private record NextState() {
    }

    private record AfterNotice() {
    }

    private record AfterDelayedNotice() {
    }
}
