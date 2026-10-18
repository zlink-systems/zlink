package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.protocol.ServiceWireConstants.FRAMEWORK_ERROR_NONE;
import static systems.zlink.framework.runtime.protocol.ServiceWireConstants.FRAMEWORK_ERROR_WORKER_TIMED_OUT;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpotRelocationReplyRoutesTest {
    @Test
    void canonicalFailedTerminalUsesFailureCapabilityOnce() {
        verifyCanonicalFailedTerminal(false, false);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void canonicalFailedTerminalCanBeAcceptedAfterFailureCallbackFails(boolean synchronous) {
        verifyCanonicalFailedTerminal(true, synchronous);
    }

    private void verifyCanonicalFailedTerminal(boolean failFirst, boolean synchronous) {
        var routes = new ZLinkSpotRelocationReplyRoutes();
        byte[] accepted =
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-1", 51, "actor.query", Map.of(), new byte[] {2});
        AtomicInteger successes = new AtomicInteger();
        AtomicInteger failures = new AtomicInteger();
        AtomicInteger releases = new AtomicInteger();
        var firstDelivery = new CompletableFuture<Void>();
        var transportFailure = new IllegalStateException("reply transport failed");
        var registration =
                routes.registerActor(
                        accepted,
                        "actor-1",
                        1,
                        parts -> {
                            successes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        },
                        failure -> {
                            assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, failure.kind());
                            assertEquals(
                                    (int) FRAMEWORK_ERROR_WORKER_TIMED_OUT,
                                    ZLinkRequestFailureMapping.causeCode(failure));
                            if (failures.incrementAndGet() == 1 && failFirst) {
                                if (synchronous) throw transportFailure;
                                return firstDelivery;
                            }
                            return CompletableFuture.completedFuture(null);
                        },
                        releases::incrementAndGet);
        registration.releaseForRelocation();
        registration.releaseForRelocation();
        assertEquals(1, releases.get());
        RoutingId target = RoutingId.from("target-node");
        routes.bindActorCommitted(
                List.of(accepted),
                target,
                7,
                new ZLinkSpotRelocationReplyRoutes.CommittedFence("authority-key", 2, 9));
        var codec = new ZLinkServiceRelocationWireCodec();
        var relay =
                codec.decodeReplyRelay(
                        codec.encodeReplyRelay(
                                new ZLinkServiceRelocationWireCodec.ReplyRelay(
                                        new ZLinkServiceRelocationWireCodec.Operation(1, 2),
                                        51,
                                        new ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
                                        9,
                                        new ZLinkServiceRelocationWireCodec.CoordinatorFence(
                                                "target-owner", 3, target, 7, "store-version"),
                                        2,
                                        10,
                                        RequestResult.INTERNAL_ERROR.value(),
                                        (int) FRAMEWORK_ERROR_WORKER_TIMED_OUT)));
        assertNull(routes.lookupCanonical(relay, RoutingId.from("wrong-target")));
        var route = routes.lookupCanonical(relay, target);
        assertNotNull(route);
        var staleRoute =
                new ZLinkSpotRelocationReplyRoutes.CanonicalRoute(
                        route.operation(),
                        "stale-authority",
                        route.objectId(),
                        route.objectGeneration(),
                        route.sourceOwnerId(),
                        route.sourceOwnerLeaseGeneration(),
                        route.sourceNodeRid(),
                        route.sourceNodeGeneration(),
                        route.replyRouteId(),
                        route.participantId());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.NOT_ACKNOWLEDGED,
                routes.deliverCanonical(
                                staleRoute, List.of(), relay.terminalResult(), relay.failureCode())
                        .toCompletableFuture()
                        .join());
        assertEquals(0, successes.get());
        assertEquals(0, failures.get());
        var pending =
                routes.deliverCanonical(
                        route, List.of(), relay.terminalResult(), relay.failureCode());
        if (failFirst) {
            if (!synchronous) {
                assertTrue(!pending.toCompletableFuture().isDone());
                assertEquals(
                        ZLinkSpotRelocationReplyRoutes.Ack.NOT_ACKNOWLEDGED,
                        routes.deliverCanonical(
                                        route,
                                        List.of(),
                                        relay.terminalResult(),
                                        relay.failureCode())
                                .toCompletableFuture()
                                .join());
                firstDelivery.completeExceptionally(transportFailure);
            }
            var rejected =
                    org.junit.jupiter.api.Assertions.assertThrows(
                            java.util.concurrent.CompletionException.class,
                            () -> pending.toCompletableFuture().join());
            org.junit.jupiter.api.Assertions.assertSame(transportFailure, rejected.getCause());
            assertEquals(
                    ZLinkSpotRelocationReplyRoutes.Ack.TERMINAL_RECEIVED,
                    routes.deliverCanonical(
                                    route, List.of(), relay.terminalResult(), relay.failureCode())
                            .toCompletableFuture()
                            .join());
        } else {
            assertEquals(
                    ZLinkSpotRelocationReplyRoutes.Ack.TERMINAL_RECEIVED,
                    pending.toCompletableFuture().join());
        }
        assertEquals(failFirst ? 2 : 1, failures.get());
        assertEquals(0, successes.get());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL,
                routes.deliverCanonical(
                                route, List.of(), relay.terminalResult(), relay.failureCode())
                        .toCompletableFuture()
                        .join());
        assertEquals(failFirst ? 2 : 1, failures.get());
        assertEquals(1, routes.size(), "terminal capability stays retained for duplicate ACKs");
    }

    @Test
    void unavailableNotificationFailureDoesNotSkipOtherPendingRoutes() {
        var routes = new ZLinkSpotRelocationReplyRoutes();
        var deliveryFailure = new IllegalStateException("reply transport failed");
        AtomicInteger notifications = new AtomicInteger();
        for (long requestId : new long[] {41, 42}) {
            byte[] accepted =
                    ZLinkAcceptedJournalTestRecords.spot(
                            "source-spot",
                            "room-1",
                            requestId,
                            "room.query",
                            Map.of(),
                            new byte[] {1},
                            requestId);
            var received =
                    new ZLinkBackendReceived(
                            ZLinkBackendRequestResult.OK,
                            Optional.of(RoutingId.from("journal-node")),
                            Optional.of("room-1"),
                            Optional.of(requestId),
                            new byte[0],
                            accepted,
                            List.of(),
                            parts -> {
                                notifications.incrementAndGet();
                                parts.forEach(Message::close);
                                if (requestId == 41) throw deliveryFailure;
                            },
                            () -> {});
            routes.register(accepted, received, "room-1", 1).releaseForRelocation();
        }
        var failure =
                org.junit.jupiter.api.Assertions.assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () ->
                                routes.failUnavailable(false, "room-1", 1)
                                        .toCompletableFuture()
                                        .join());
        org.junit.jupiter.api.Assertions.assertSame(deliveryFailure, failure.getCause());
        assertEquals(2, notifications.get());
    }

    @Test
    void highBitReplyRouteIdIsAnOpaqueU64() {
        long highBit = Long.MIN_VALUE;
        var relay =
                new ZLinkSpotRelocationReplyRoutes.Relay(
                        new ZLinkSpotRelocationReplyRoutes.OperationId(1, 2),
                        highBit,
                        "room-1",
                        1,
                        "journal-owner",
                        1,
                        RoutingId.from("journal-node"),
                        1,
                        7,
                        9,
                        0,
                        List.of());
        assertEquals(highBit, relay.replyRouteId());
    }

    @Test
    void capturedReplySurvivesReceiveReleaseAndAcknowledgesExactlyOnce() {
        byte[] accepted =
                ZLinkAcceptedJournalTestRecords.spot(
                        "source-spot", "room-1", 41, "room.query", Map.of(), new byte[] {1});
        AtomicBoolean receiveClosed = new AtomicBoolean();
        AtomicInteger replies = new AtomicInteger();
        var received =
                new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.of(RoutingId.from("journal-node")),
                        Optional.of("room-1"),
                        Optional.of(41L),
                        new byte[0],
                        accepted,
                        List.of(),
                        parts -> {
                            replies.incrementAndGet();
                            parts.forEach(Message::close);
                        },
                        () -> receiveClosed.set(true));
        var routes = new ZLinkSpotRelocationReplyRoutes();
        var registration = routes.register(accepted, received, "room-1", 1);
        RoutingId target = RoutingId.from("target-node");

        registration.releaseForRelocation();
        routes.bindCommitted(List.of(accepted), target, 7, 9);
        var relay =
                new ZLinkSpotRelocationReplyRoutes.Relay(
                        new ZLinkSpotRelocationReplyRoutes.OperationId(1, 2),
                        41,
                        "room-1",
                        1,
                        "journal-owner",
                        1,
                        RoutingId.from("journal-node"),
                        1,
                        7,
                        9,
                        0,
                        List.of(new byte[] {9, 8, 7}));

        assertTrue(receiveClosed.get());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.TERMINAL_RECEIVED,
                routes.relay(relay, target).toCompletableFuture().join());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL,
                routes.relay(relay, target).toCompletableFuture().join());
        assertEquals(1, replies.get());
        assertEquals(1, routes.size());
    }

    @Test
    void wrongTransportSourceCannotConsumeCapturedReply() {
        byte[] accepted =
                ZLinkAcceptedJournalTestRecords.spot(
                        "source-spot", "room-1", 41, "room.query", Map.of(), new byte[] {1});
        AtomicInteger replies = new AtomicInteger();
        var received =
                new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.of(RoutingId.from("journal-node")),
                        Optional.of("room-1"),
                        Optional.of(41L),
                        new byte[0],
                        accepted,
                        List.of(),
                        parts -> replies.incrementAndGet(),
                        () -> {});
        var routes = new ZLinkSpotRelocationReplyRoutes();
        routes.register(accepted, received, "room-1", 1).releaseForRelocation();
        RoutingId target = RoutingId.from("target-node");
        routes.bindCommitted(List.of(accepted), target, 7, 9);

        var ack =
                routes.relay(
                                new ZLinkSpotRelocationReplyRoutes.Relay(
                                        new ZLinkSpotRelocationReplyRoutes.OperationId(1, 2),
                                        41,
                                        "room-1",
                                        1,
                                        "journal-owner",
                                        1,
                                        RoutingId.from("journal-node"),
                                        1,
                                        7,
                                        9,
                                        0,
                                        List.of(new byte[] {1})),
                                RoutingId.from("spoofed-target"))
                        .toCompletableFuture()
                        .join();

        assertEquals(ZLinkSpotRelocationReplyRoutes.Ack.NOT_ACKNOWLEDGED, ack);
        assertEquals(0, replies.get());
    }

    @Test
    void canonicalLookupHitsOnLandingNodeWhenRequestSourceFenceDiffers() {
        //  The reply capability lands on the relocation source node while the
        //  frozen record's request-source fence names another node. The relay
        //  sent by the relocation target must still resolve the route.
        byte[] accepted =
                ZLinkAcceptedJournalTestRecords.spot(
                        "source-spot", "room-1", 41, "room.query", Map.of(), new byte[] {1});
        AtomicInteger replies = new AtomicInteger();
        var received =
                new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.of(RoutingId.from("journal-node")),
                        Optional.of("room-1"),
                        Optional.of(41L),
                        new byte[0],
                        accepted,
                        List.of(),
                        parts -> {
                            replies.incrementAndGet();
                            parts.forEach(Message::close);
                        },
                        () -> {});
        var routes = new ZLinkSpotRelocationReplyRoutes();
        routes.register(accepted, received, "room-1", 1).releaseForRelocation();
        RoutingId target = RoutingId.from("target-node");
        routes.bindCommitted(
                List.of(accepted),
                target,
                7,
                new ZLinkSpotRelocationReplyRoutes.CommittedFence("authority-key", 2, 9));
        var relay =
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec
                        .ReplyRelay(
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.Operation(1, 2),
                        41,
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
                        9,
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.CoordinatorFence(
                                "target-owner", 3, target, 7, "store-version"),
                        2,
                        10,
                        0,
                        0);

        var route = routes.lookupCanonical(relay, target);

        assertNotNull(route, "relay from the relocation target must land on this node");
        assertEquals(
                RoutingId.from("journal-node"),
                route.sourceNodeRid(),
                "the ACK keeps the request-source fence of the frozen record");
        assertNull(
                routes.lookupCanonical(relay, RoutingId.from("journal-node")),
                "a relay that does not arrive from the relocation target misses");
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.TERMINAL_RECEIVED,
                routes.deliverCanonical(
                                route,
                                List.of(new byte[] {1}),
                                RequestResult.OK.value(),
                                (int) FRAMEWORK_ERROR_NONE)
                        .toCompletableFuture()
                        .join());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL,
                routes.deliverCanonical(
                                route,
                                List.of(new byte[] {1}),
                                RequestResult.OK.value(),
                                (int) FRAMEWORK_ERROR_NONE)
                        .toCompletableFuture()
                        .join());
        assertEquals(1, replies.get());
    }

    @Test
    void actorReplyUsesSameClosedAckAndExactFenceComponent() {
        byte[] accepted =
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-1", 51, "actor.query", Map.of(), new byte[] {2});
        AtomicBoolean released = new AtomicBoolean();
        AtomicInteger replies = new AtomicInteger();
        var routes = new ZLinkSpotRelocationReplyRoutes();
        routes.registerActor(
                        accepted,
                        "actor-1",
                        1,
                        parts -> {
                            assertArrayEquals(new byte[] {7}, parts.getFirst());
                            replies.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        },
                        CompletableFuture::failedFuture,
                        () -> released.set(true))
                .releaseForRelocation();
        RoutingId target = RoutingId.from("actor-target");
        routes.bindActorCommitted(List.of(accepted), target, 5, 6);
        var relay =
                new ZLinkSpotRelocationReplyRoutes.Relay(
                        new ZLinkSpotRelocationReplyRoutes.OperationId(1, 2),
                        51,
                        "actor-1",
                        1,
                        "journal-owner",
                        1,
                        RoutingId.from("journal-node"),
                        1,
                        5,
                        6,
                        0,
                        List.of(new byte[] {7}));

        assertTrue(released.get());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.TERMINAL_RECEIVED,
                routes.relay(relay, target).toCompletableFuture().join());
        assertEquals(
                ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL,
                routes.relay(relay, target).toCompletableFuture().join());
        assertEquals(1, replies.get());
    }
}
