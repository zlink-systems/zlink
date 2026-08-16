package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Arrays;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;

final class ZLinkServiceM6BActorCreateWireCodecTest {
    private final ZLinkServiceM6BWireCodec codec =
        new ZLinkServiceM6BWireCodec();

    @Test
    void command49RoundTripsSourceOperationDeadlineAndReservation() {
        var command = new ZLinkServiceM6BWireCodec.ActorCreate(
            71,
            11,
            12,
            RoutingId.from("source"),
            4,
            "actor-1",
            "player",
            reservation(),
            1_900_000_000_000L);

        var decoded = codec.decodeActorCreateHeader(
            codec.encodeActorCreateHeader(command));

        assertEquals(command, decoded);
    }

    @Test
    void durableTerminalIsCorrelationFreeAndReplyUsesCurrentCorrelation() {
        byte[] application = new ZLinkServiceM6AWireCodec()
            .encodeApplicationPayload(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "created",
                    "application/json",
                    new byte[] {1, 2, 3}));
        var terminal =
            new ZLinkServiceM6BWireCodec.ActorCreationTerminal(
                0,
                0,
                new ZLinkServiceM6BWireCodec.ActorCreateTerminal(
                    ZLinkServiceM6BWireCodec.ActorCreateResult.CREATED,
                    new ActorRef(
                        "actor-1",
                        9,
                        "game",
                        RoutingId.from("target"))),
                application);
        byte[] durable =
            codec.encodeCreationOperationTerminal(terminal);

        var restored =
            codec.decodeCreationOperationTerminal(durable);
        byte[] first = codec.encodeActorCreateReply(101, restored);
        byte[] retry = codec.encodeActorCreateReply(202, restored);

        assertNotEquals(
            codec.decodeActorCreateReply(first, "game").correlation(),
            codec.decodeActorCreateReply(retry, "game").correlation());
        assertEquals(
            terminal.creation(),
            codec.decodeActorCreateReply(first, "game").terminal().creation());
        assertArrayEquals(
            application,
            restored.applicationPayloadFrame());
        assertArrayEquals(
            durable,
            codec.encodeCreationOperationTerminal(restored));
    }

    @Test
    void malformedActorCreateReplyThrowsIllegalArgumentForProtocolError() {
        //  completeActorCreate converts any IllegalArgumentException (which
        //  every codec malformed-wire ZLinkServiceWireException derives from)
        //  into ZLinkFrameworkException(PROTOCOL_ERROR): a reply that can't be
        //  processed is a ProtocolError (spec 32-framework-error-model:91-92).
        //  This pins the inheritance that conversion relies on.
        assertTrue(IllegalArgumentException.class.isAssignableFrom(
            ZLinkServiceWireException.class));
        var terminal =
            new ZLinkServiceM6BWireCodec.ActorCreationTerminal(
                0,
                0,
                new ZLinkServiceM6BWireCodec.ActorCreateTerminal(
                    ZLinkServiceM6BWireCodec.ActorCreateResult.CREATED,
                    new ActorRef(
                        "actor-1",
                        9,
                        "game",
                        RoutingId.from("target"))),
                null);
        byte[] reply = codec.encodeActorCreateReply(101, terminal);
        ZLinkServiceWireException failure = assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeActorCreateReply(
                Arrays.copyOf(reply, reply.length - 1), "game"));
        assertTrue(failure instanceof IllegalArgumentException);
    }

    private static ZLinkServiceM6BWireCodec.ReservationFence
        reservation() {
        return new ZLinkServiceM6BWireCodec.ReservationFence(
            "reservation",
            "store-version",
            9,
            10,
            RoutingId.from("target"),
            5,
            "owner",
            6,
            1);
    }
}
