package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;

import java.time.Clock;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkMessageContextReceivedAtTest {
    private static final Instant ACCEPTED_AT = Instant.parse("2026-11-02T01:23:45.678Z");
    private static final Instant AFTER_QUEUE_WAIT = Instant.parse("2026-11-02T01:23:55.678Z");

    @RepeatedTest(100)
    void remoteRequestContextKeepsTheIngressTimeAfterQueueWait() {
        var clock = new MutableClock(ACCEPTED_AT);
        try (var received =
                new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.empty(),
                        Optional.of("spot-a"),
                        Optional.of(3L),
                        new byte[0],
                        new byte[0],
                        List.of(Message.from(new byte[0])),
                        null,
                        null,
                        "application/json",
                        clock)) {
            clock.set(AFTER_QUEUE_WAIT);
            var context =
                    new ZLinkSpotRequestHandlerContext(
                            "request.packet", "application/json", Map.of(), received.receivedAt());

            assertEquals(ACCEPTED_AT, context.receivedAt());
        }
    }

    @RepeatedTest(100)
    void actorRequestContextKeepsTheAcceptedSubmissionTimeAfterQueueWait() {
        var clock = new MutableClock(ACCEPTED_AT);
        try (var received =
                new ZLinkBackendActorReceived(
                        new ZLinkBackendActorRef(RoutingId.from("node-a"), "actor-a", 1),
                        RoutingId.from("node-a"),
                        RoutingId.from("session-a"),
                        Optional.of(4L),
                        7,
                        0,
                        Message.from(new byte[0]),
                        false,
                        new byte[0],
                        "application/json",
                        clock)) {
            clock.set(AFTER_QUEUE_WAIT);
            try (var copy = received.copy()) {
                var context =
                        new ZLinkSpotActorRequestHandlerContext(
                                "actor.request", "application/json", Map.of(), copy.receivedAt());

                assertEquals(ACCEPTED_AT, context.receivedAt());
            }
        }
    }

    private static final class MutableClock extends Clock {
        private final AtomicReference<Instant> instant;

        private MutableClock(Instant instant) {
            this.instant = new AtomicReference<>(instant);
        }

        @Override
        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        @Override
        public Clock withZone(ZoneId zone) {
            return this;
        }

        @Override
        public Instant instant() {
            return instant.get();
        }

        private void set(Instant value) {
            instant.set(value);
        }
    }
}
