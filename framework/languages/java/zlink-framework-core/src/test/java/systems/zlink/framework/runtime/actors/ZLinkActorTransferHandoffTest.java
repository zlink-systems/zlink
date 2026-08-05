package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.util.List;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkActorTransferHandoffTest {
    @Test
    void inFlightHandoffKeepsArrivalOrder() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.begin("actor");
        capture(handoff, "P1", Map.of());
        capture(handoff, "P2", Map.of());
        capture(handoff, "P3", Map.of());

        List<ZLinkActorHandoffPacket> backlog = handoff.take("actor");
        assertEquals(List.of("P1", "P2", "P3"),
            backlog.stream().map(packet -> packet.header().packetName()).toList());
        assertTrue(backlog.get(0).arrivalIndex() < backlog.get(1).arrivalIndex());
        assertTrue(backlog.get(1).arrivalIndex() < backlog.get(2).arrivalIndex());
        backlog.forEach(ZLinkActorHandoffPacket::close);
    }

    @Test
    void packetsAfterCommitSnapshotRemainBehindSnapshot() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.begin("actor");
        capture(handoff, "B1", Map.of());
        capture(handoff, "B2", Map.of());
        List<ZLinkActorHandoffPacket> committed = handoff.take("actor");
        capture(handoff, "D1", Map.of());
        List<ZLinkActorHandoffPacket> trailing = handoff.finish("actor");

        assertEquals(List.of("B1", "B2", "D1"),
            java.util.stream.Stream.concat(committed.stream(), trailing.stream())
                .map(packet -> packet.header().packetName()).toList());
        committed.forEach(ZLinkActorHandoffPacket::close);
        trailing.forEach(ZLinkActorHandoffPacket::close);
    }

    @Test
    void precommitAbortRestoresSnapshotAndTrailingPacketsInArrivalOrder() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.begin("actor");
        capture(handoff, "B1", Map.of());
        capture(handoff, "B2", Map.of());
        List<ZLinkActorHandoffPacket> committed = handoff.take("actor");
        capture(handoff, "D1", Map.of());

        List<ZLinkActorHandoffPacket> restored =
            handoff.takeForRestore("actor", committed);
        assertEquals(
            List.of("B1", "B2", "D1"),
            restored.stream()
                .map(packet -> packet.header().packetName())
                .toList());
        assertEquals(List.of(), handoff.finish("actor"));
        restored.forEach(ZLinkActorHandoffPacket::close);
        handoff.close();
    }

    @Test
    void boundSessionRouteMetadataSurvivesHandoff() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.begin("actor");
        capture(handoff, "S1", Map.of("actor-id", "actor", "session", "bound-1"));

        ZLinkActorHandoffPacket packet = handoff.take("actor").get(0);
        assertEquals("actor", packet.header().metadata().get("actor-id"));
        assertEquals("bound-1", packet.header().metadata().get("session"));
        packet.close();
    }

    @Test
    void canonicalAcceptedJournalSurvivesHandoff() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.begin("actor");
        byte[] journal = new byte[] {9, 8, 7, 6};
        try (Message payload = Message.from("payload")) {
            handoff.capture(
                "actor",
                new ZLinkStreamHeader("DeferredSend", Map.of(), Optional.empty()),
                payload,
                null,
                journal);
        }

        ZLinkActorHandoffPacket packet = handoff.take("actor").get(0);
        assertArrayEquals(journal, packet.acceptedJournalRecord());
        byte[] copy = packet.acceptedJournalRecord();
        copy[0] = 0;
        assertArrayEquals(journal, packet.acceptedJournalRecord());
        packet.close();
    }

    @Test
    void messageFollowDurationRemovesRoute() throws Exception {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        CountDownLatch removed = new CountDownLatch(1);
        handoff.retain("actor", ref("source", 1), ref("target", 2),
            Duration.ofMillis(25), ignored -> removed.countDown());

        assertEquals(1, handoff.messageFollowSourceCount());
        assertTrue(removed.await(1, TimeUnit.SECONDS));
        assertEquals(0, handoff.messageFollowSourceCount());
        handoff.close();
    }

    @Test
    void repeatedRelocationReplacesMessageFollowRouteWithoutLeakingEntries() throws Exception {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        List<Long> removedTargets = new CopyOnWriteArrayList<>();
        CountDownLatch removed = new CountDownLatch(2);
        handoff.retain("actor", ref("source", 1), ref("target", 2),
            Duration.ofMillis(30), source -> {
                removedTargets.add(source.targetActorRef().generation());
                removed.countDown();
            });
        handoff.retain("actor", ref("source", 3), ref("target", 4),
            Duration.ofMillis(60), source -> {
                removedTargets.add(source.targetActorRef().generation());
                removed.countDown();
            });

        assertEquals(1, handoff.messageFollowSourceCount());
        assertEquals(4, handoff.messageFollowSource("actor").orElseThrow()
            .targetActorRef().generation());
        assertTrue(removed.await(1, TimeUnit.SECONDS));
        assertEquals(List.of(2L, 4L), removedTargets);
        assertEquals(0, handoff.messageFollowSourceCount());
        handoff.close();
    }

    @Test
    void closeRemovesOwnedMessageFollowRoutesImmediately() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        List<Long> removedTargets = new CopyOnWriteArrayList<>();
        handoff.retain("actor", ref("source", 1), ref("target", 2),
            Duration.ofMinutes(1), source ->
                removedTargets.add(source.targetActorRef().generation()));

        handoff.close();

        assertEquals(List.of(2L), removedTargets);
        assertEquals(0, handoff.messageFollowSourceCount());
    }

    @Test
    void inFlightRequestPreservesReplyCorrelationFraming() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.begin("actor");
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.of(ZLinkStreamHeaderFlag.HAS_METADATA),
            Optional.of(77L),
            "ProbeReq",
            Map.of("trace", "handoff"),
            Optional.of("corr-77"));
        ZLinkActorReplyRoute route = new ZLinkActorReplyRoute(
            ref("source", 9), RoutingId.from("caller-node"),
            RoutingId.from("caller-session"), 91L, 5);
        try (Message payload = Message.from(new byte[] {1})) {
            handoff.capture(
                "actor", header, payload, route, new byte[] {1});
        }

        ZLinkActorHandoffPacket packet = handoff.take("actor").get(0);
        assertEquals(Optional.of(77L), packet.header().requestSequence());
        assertEquals(Optional.of("corr-77"), packet.header().correlationId());
        assertEquals("handoff", packet.header().metadata().get("trace"));
        assertEquals(route, packet.replyRoute());
        packet.close();
    }

    @Test
    void committedMessageFollowRouteBoundsMessagesAndBytes() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.retain(
            "actor", ref("source", 7), ref("target", 7),
            Duration.ofMinutes(1), ignored -> { });
        List<CompletableFuture<Void>> pending = new java.util.ArrayList<>();
        for (int index = 0;
             index < ZLinkActorTransferHandoff.MAX_MESSAGE_FOLLOW_MESSAGES;
             index++) {
            CompletableFuture<Void> operation = new CompletableFuture<>();
            pending.add(operation);
            handoff.follow("actor", 7, 1, () -> operation);
        }

        assertThrows(CompletionException.class, () ->
            handoff.follow(
                    "actor", 7, 1,
                    () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture().join());
        pending.forEach(value -> value.complete(null));

        CompletableFuture<Void> bytes = new CompletableFuture<>();
        handoff.follow(
            "actor", 7, ZLinkActorTransferHandoff.MAX_MESSAGE_FOLLOW_BYTES,
            () -> bytes);
        assertThrows(CompletionException.class, () ->
            handoff.follow(
                    "actor", 7, 1,
                    () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture().join());
        bytes.complete(null);
        handoff.close();
    }

    @Test
    void messageFollowRejectsDifferentObjectGeneration() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.retain(
            "actor", ref("source", 7), ref("target", 7),
            Duration.ofMinutes(1), ignored -> { });

        assertThrows(CompletionException.class, () ->
            handoff.follow(
                    "actor", 8, 1,
                () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture().join());
        handoff.close();
    }

    @Test
    void messageFollowNoticeClaimIsSingleUseUntilExplicitlyReleased() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.retain(
            "actor", ref("source", 7), ref("target", 7),
            Duration.ofMinutes(1), ignored -> { });

        ZLinkActorTransferHandoff.MessageFollowSource source =
            handoff.messageFollowSource("actor").orElseThrow();
        assertTrue(source.tryClaimMessageFollowNotice());
        assertTrue(source.messageFollowNoticeClaimed());
        assertFalse(source.tryClaimMessageFollowNotice());

        source.releaseMessageFollowNoticeClaim();
        assertFalse(source.messageFollowNoticeClaimed());
        assertTrue(source.tryClaimMessageFollowNotice());
        handoff.close();
    }

    @Test
    void messageFollowQueueSnapshotSurvivesCompletionRelease() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        handoff.retain(
            "actor", ref("source", 7), ref("target", 7),
            Duration.ofMinutes(1), ignored -> { });
        CompletableFuture<String> operation = new CompletableFuture<>();

        CompletionStage<ZLinkActorTransferHandoff.FollowResult<String>> result =
            handoff.followWithQueueSnapshot("actor", 7, 3, () -> operation);
        ZLinkActorTransferHandoff.MessageFollowSource source =
            handoff.messageFollowSource("actor").orElseThrow();
        assertEquals(1, source.pendingMessages());
        assertEquals(3, source.pendingBytes());

        operation.complete("relayed");
        ZLinkActorTransferHandoff.FollowResult<String> completed =
            result.toCompletableFuture().join();
        assertEquals("relayed", completed.value());
        assertEquals(1, completed.queue().messages());
        assertEquals(3, completed.queue().bytes());
        assertEquals(0, source.pendingMessages());
        assertEquals(0, source.pendingBytes());
        handoff.close();
    }

    @Test
    void messageFollowRetainsTargetRouteFenceAtInstallation() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute =
            new ZLinkServiceMessageFollowWireCodec.ActorRoute(
                "actor", 7, RoutingId.from("target"), 11, 13, 17);
        SpotTransportAddress targetAddress = new SpotTransportAddress(
            "router", RoutingId.from("target"), "spot", 7, 11, 13, 17,
            systems.zlink.framework.spots.ZLinkSpotKind.USER);
        handoff.retain(
            "actor", ref("source", 7), ref("target", 7), targetAddress,
            targetRoute, Duration.ofMinutes(1), ignored -> { });

        assertEquals(
            targetRoute,
            handoff.messageFollowSource("actor").orElseThrow().targetRoute());
        handoff.close();
    }

    @Test
    void messageFollowRejectsAddressWithoutTargetRouteFence() {
        ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
        SpotTransportAddress targetAddress = new SpotTransportAddress(
            "router", RoutingId.from("target"), "spot", 7, 11, 13, 17,
            systems.zlink.framework.spots.ZLinkSpotKind.USER);

        assertThrows(IllegalArgumentException.class, () -> handoff.retain(
            "actor", ref("source", 7), ref("target", 7), targetAddress,
            null, Duration.ofMinutes(1), ignored -> { }));
        handoff.close();
    }

    @Test
    void messageFollowTargetRouteMatchesBackendActorRefByFields() {
        RoutingId targetNode = RoutingId.from("target");
        ZLinkStoreLocationResolvers.ActorRoute route =
            new ZLinkStoreLocationResolvers.ActorRoute(
                new systems.zlink.framework.actors.ActorRef(
                    "actor", 7, "mesh", targetNode),
                systems.zlink.framework.spots.ZLinkSpotKind.USER,
                "spot",
                "mesh",
                targetNode,
                11,
                13,
                17);
        ZLinkBackendActorRef targetActor =
            new ZLinkBackendActorRef(targetNode, "actor", 7);
        SpotTransportAddress targetAddress = new SpotTransportAddress(
            "router", targetNode, "spot", 7, 11, 13, 17,
            systems.zlink.framework.spots.ZLinkSpotKind.USER);

        assertTrue(ZLinkActorRuntime.messageFollowTargetRouteMatches(
            route, targetActor, targetAddress));
        assertFalse(ZLinkActorRuntime.messageFollowTargetRouteMatches(
            route,
            new ZLinkBackendActorRef(targetNode, "actor", 8),
            targetAddress));
    }

    private static void capture(
        ZLinkActorTransferHandoff handoff,
        String packetName,
        Map<String, String> metadata) {
        try (Message payload = Message.from(packetName.getBytes(java.nio.charset.StandardCharsets.UTF_8))) {
            handoff.capture(
                "actor",
                new ZLinkStreamHeader(packetName, metadata, Optional.empty()),
                payload,
                null,
                new byte[] {1});
        }
    }

    private static ZLinkBackendActorRef ref(String node, long generation) {
        return new ZLinkBackendActorRef(RoutingId.from(node), "actor", generation);
    }
}
