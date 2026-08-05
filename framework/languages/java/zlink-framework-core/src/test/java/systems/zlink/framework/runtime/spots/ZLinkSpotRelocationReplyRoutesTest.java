package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;

final class ZLinkSpotRelocationReplyRoutesTest {
    @Test
    void capturedReplySurvivesReceiveReleaseAndAcknowledgesExactlyOnce() {
        byte[] accepted = ZLinkAcceptedJournalTestRecords.spot(
            "source-spot",
            "room-1",
            41,
            "room.query",
            Map.of(),
            new byte[] {1});
        AtomicBoolean receiveClosed = new AtomicBoolean();
        AtomicInteger replies = new AtomicInteger();
        var received = new ZLinkBackendReceived(
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
        var registration = routes.register(
            accepted, received, "room-1", 1);
        RoutingId target = RoutingId.from("target-node");

        registration.releaseForRelocation();
        routes.bindCommitted(List.of(accepted), target, 7, 9);
        var relay = new ZLinkSpotRelocationReplyRoutes.Relay(
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
        byte[] accepted = ZLinkAcceptedJournalTestRecords.spot(
            "source-spot", "room-1", 41, "room.query", Map.of(),
            new byte[] {1});
        AtomicInteger replies = new AtomicInteger();
        var received = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("journal-node")),
            Optional.of("room-1"),
            Optional.of(41L),
            new byte[0],
            accepted,
            List.of(),
            parts -> replies.incrementAndGet(),
            () -> { });
        var routes = new ZLinkSpotRelocationReplyRoutes();
        routes.register(accepted, received, "room-1", 1)
            .releaseForRelocation();
        RoutingId target = RoutingId.from("target-node");
        routes.bindCommitted(List.of(accepted), target, 7, 9);

        var ack = routes.relay(
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
            RoutingId.from("spoofed-target")).toCompletableFuture().join();

        assertEquals(ZLinkSpotRelocationReplyRoutes.Ack.NOT_ACKNOWLEDGED, ack);
        assertEquals(0, replies.get());
    }

    @Test
    void actorReplyUsesSameClosedAckAndExactFenceComponent() {
        byte[] accepted = ZLinkAcceptedJournalTestRecords.actor(
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
                    return java.util.concurrent.CompletableFuture
                        .completedFuture(null);
                },
                () -> released.set(true))
            .releaseForRelocation();
        RoutingId target = RoutingId.from("actor-target");
        routes.bindActorCommitted(List.of(accepted), target, 5, 6);
        var relay = new ZLinkSpotRelocationReplyRoutes.Relay(
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
