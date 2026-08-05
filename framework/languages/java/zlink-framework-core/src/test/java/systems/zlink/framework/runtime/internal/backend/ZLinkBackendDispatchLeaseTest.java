package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

final class ZLinkBackendDispatchLeaseTest {
    @Test
    void routePartsCanBeReleasedBeforeTheHandlerLeaseTerminates() {
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(32);
        ZLinkInboundDispatchBudget.Lease lease = budget.track(7);
        ZLinkBackendReceived received = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("source")),
            Optional.of("spot"),
            Optional.empty(),
            new byte[0],
            new byte[0],
            List.of(Message.from("payload")),
            null,
            () -> { },
            lease);

        received.closeParts();
        assertEquals(7, budget.snapshot().pendingPayloadBytes());

        received.closeAdmission();
        received.closeAdmission();
        assertEquals(0, budget.snapshot().pendingPayloadBytes());
    }

    @Test
    void actorMessageCloseReleasesItsTransferredLease() {
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(32);
        ZLinkInboundDispatchBudget.Lease lease = budget.track(9);
        ZLinkBackendActorReceived received = new ZLinkBackendActorReceived(
            new ZLinkBackendActorRef(
                RoutingId.from("node"), "actor", 1),
            RoutingId.from("source"),
            null,
            Optional.empty(),
            0,
            0,
            Message.from("payload"),
            false,
            new byte[0],
            lease);

        received.close();
        received.close();

        assertEquals(0, budget.snapshot().pendingPayloadBytes());
    }

    @Test
    void receivedRecordsRetainWireContentType() {
        ZLinkBackendReceived route = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            new byte[0],
            new byte[0],
            List.of(Message.from("payload")),
            null,
            () -> { },
            "application/example",
            null);
        ZLinkBackendActorReceived actor = new ZLinkBackendActorReceived(
            new ZLinkBackendActorRef(RoutingId.from("node"), "actor", 1),
            RoutingId.from("source"),
            null,
            Optional.empty(),
            0,
            0,
            Message.from("payload"),
            false,
            new byte[0],
            "application/example",
            null);
        ZLinkBackendTopicMessage topic = new ZLinkBackendTopicMessage(
            Optional.empty(),
            "channel",
            "topic",
            new byte[0],
            List.of(Message.from("payload")),
            "application/example",
            null);

        try {
            assertEquals("application/example", route.contentType());
            assertEquals("application/example", actor.contentType());
            assertEquals("application/example", topic.contentType());
        } finally {
            route.close();
            actor.close();
            topic.parts().forEach(Message::close);
        }
    }
}
