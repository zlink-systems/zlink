package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.Arrays;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.ActorIdentity;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.RelocationCoordinatorFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.RelocationIdentity;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.RelocationRole;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionOwnerFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationRoute;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationRouteAction;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationRouted;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationRouteIntent;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.ActorRouteFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationSeal;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationSealed;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertFalse;

final class ZLinkServiceM6BSessionRelocationWireCodecTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId SESSION = RoutingId.from("session");

    private final ZLinkServiceM6BWireCodec codec = new ZLinkServiceM6BWireCodec();

    @Test
    void command44CommitRoundTripsEveryAdmissionFence() {
        SessionRelocationRoute route = commit();

        assertEquals(route, codec.decodeSessionRelocationRoute(
            codec.encodeSessionRelocationRoute(route)));
    }

    @Test
    void command45AckRoundTripsHighWaterAndExactBinding() {
        SessionRelocationRoute route = commit();
        SessionRelocationRouted ack = new SessionRelocationRouted(
            route.relocation(), route.coordinator(), route.actor(),
            route.session(), route.action(),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
            route.currentAuthorityOwnerGeneration(),
            route.lastAcceptedSessionSequence());

        assertEquals(ack, codec.decodeSessionRelocationRouted(
            codec.encodeSessionRelocationRouted(ack)));

        //  Every result value round-trips, and the byte that carries it is
        //  validated on decode.
        for (var result : ZLinkServiceM6BWireCodec
                .SessionRelocationRouteResult.values()) {
            SessionRelocationRouted refused = new SessionRelocationRouted(
                route.relocation(), route.coordinator(), route.actor(),
                route.session(), route.action(), result,
                route.currentAuthorityOwnerGeneration(),
                route.lastAcceptedSessionSequence());
            byte[] encoded = codec.encodeSessionRelocationRouted(refused);
            assertEquals(refused, codec.decodeSessionRelocationRouted(encoded));
        }
        byte[] frame = codec.encodeSessionRelocationRouted(ack);
        frame[frame.length - 17] = 9;
        assertThrows(RuntimeException.class,
            () -> codec.decodeSessionRelocationRouted(frame));
    }

    @Test
    void directJoinRouteIntentMaterializesWithTheCommittedOwnerGeneration() {
        SessionRelocationRoute route = commit();
        SessionRelocationRouteIntent intent = new SessionRelocationRouteIntent(
            route.relocation(),
            route.coordinator(),
            route.senderRole(),
            route.actor(),
            route.session(),
            route.action(),
            route.previousAuthorityOwnerGeneration(),
            route.targetNodeRid(),
            route.targetNodeGeneration(),
            route.lastAcceptedSessionSequence());

        SessionRelocationRoute decoded = codec
            .decodeSessionRelocationRouteIntent(
                codec.encodeSessionRelocationRouteIntent(intent))
            .materialize(17);

        assertEquals(11, decoded.previousAuthorityOwnerGeneration());
        assertEquals(17, decoded.currentAuthorityOwnerGeneration());
        assertEquals(TARGET, decoded.targetNodeRid());
    }

    @Test
    void command44RejectsTrailingBytesAndNonMonotonicOwnerGeneration() {
        byte[] valid = codec.encodeSessionRelocationRoute(commit());
        assertThrows(ZLinkServiceWireException.class,
            () -> codec.decodeSessionRelocationRoute(
                Arrays.copyOf(valid, valid.length + 1)));
        assertThrows(ZLinkServiceWireException.class,
            () -> new SessionRelocationRoute(
                relocation(), coordinator(), RelocationRole.TARGET,
                actor(), session(), SessionRelocationRouteAction.COMMIT,
                11, 11, TARGET, 4, 29));
    }

    @Test
    void command42AndCommand43RoundTripEveryExactFence() {
        SessionRelocationSeal seal = seal();
        assertEquals(seal, codec.decodeSessionRelocationSeal(
            codec.encodeSessionRelocationSeal(seal)));

        SessionRelocationSealed sealed = new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(),
            seal.session(), 41);
        assertEquals(sealed, codec.decodeSessionRelocationSealed(
            codec.encodeSessionRelocationSealed(sealed)));
        assertTrue(sealed.echoes(seal));
        assertFalse(new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(),
            new SessionOwnerFence(SOURCE, 2, "session-owner", 8, SESSION, 7),
            41).echoes(seal));
    }

    //  A Session that never forwarded a bound message has high-water zero;
    //  `lastAcceptedSessionSequence` is `ordinal-or-zero` in the schema and
    //  `append_u64` in the C++ encoder, so zero must survive the round trip.
    @Test
    void command43CarriesAZeroHighWaterAndRejectsTrailingBytes() {
        SessionRelocationSeal seal = seal();
        SessionRelocationSealed sealed = new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(),
            seal.session(), 0);
        byte[] frame = codec.encodeSessionRelocationSealed(sealed);

        assertEquals(0,
            codec.decodeSessionRelocationSealed(frame)
                .lastAcceptedSessionSequence());
        assertThrows(ZLinkServiceWireException.class,
            () -> codec.decodeSessionRelocationSealed(
                Arrays.copyOf(frame, frame.length + 1)));
        assertThrows(ZLinkServiceWireException.class,
            () -> codec.decodeSessionRelocationSeal(frame));
        assertThrows(ZLinkServiceWireException.class,
            () -> new SessionRelocationSeal(
                relocation(), coordinator(), RelocationRole.TARGET,
                actorRoute(), session()));
    }

    /**
     * Byte-for-byte cross-language check. No golden fixture under
     * `runtime/protocol/golden/` covers commands 42/43, so the expected bytes
     * are laid out here directly from the C++ reference encoders
     * `encode_session_relocation_seal` / `encode_session_relocation_sealed`
     * (`framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:1139`
     * and `:1238`) and from command 42/43 in
     * `runtime/protocol/service-wire-v1.schema.json`.
     */
    @Test
    void command42And43MatchTheCppByteLayout() {
        SessionRelocationSeal seal = seal();

        Bytes expected42 = new Bytes()
            .u8(90).u8(77).u8(1).u8(42).u8(0)
            .u64(7).u64(9)
            .text("coordinator").u64(3).rid(SOURCE).u64(2)
            .text16("store-v17")
            .u8(1)
            .text("actor-1").u64(5).rid(TARGET).u64(4).u64(11).u64(13)
            .rid(SOURCE).u64(2).text("session-owner").u64(8)
            .rid(SESSION).u64(6);
        assertArrayEquals(expected42.toByteArray(),
            codec.encodeSessionRelocationSeal(seal));

        SessionRelocationSealed sealed = new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(),
            seal.session(), 41);
        Bytes expected43 = new Bytes()
            .u8(90).u8(77).u8(1).u8(43).u8(0)
            .u64(7).u64(9)
            .text("coordinator").u64(3).rid(SOURCE).u64(2)
            .text16("store-v17")
            .text("actor-1").u64(5).rid(TARGET).u64(4).u64(11).u64(13)
            .rid(SOURCE).u64(2).text("session-owner").u64(8)
            .rid(SESSION).u64(6)
            .u64(41);
        assertArrayEquals(expected43.toByteArray(),
            codec.encodeSessionRelocationSealed(sealed));
    }

    private static SessionRelocationSeal seal() {
        return new SessionRelocationSeal(
            relocation(), coordinator(), RelocationRole.SOURCE,
            actorRoute(), session());
    }

    private static ActorRouteFence actorRoute() {
        return new ActorRouteFence(
            new ZLinkBackendActorRef(TARGET, "actor-1", 5), 4, 11, 13);
    }

    private static final class Bytes {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();

        Bytes u8(int value) {
            output.write(value);
            return this;
        }

        Bytes u64(long value) {
            output.writeBytes(ByteBuffer.allocate(Long.BYTES)
                .order(ByteOrder.BIG_ENDIAN).putLong(value).array());
            return this;
        }

        Bytes text(String value) {
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            output.write(bytes.length);
            output.writeBytes(bytes);
            return this;
        }

        Bytes text16(String value) {
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            output.write((bytes.length >>> 8) & 0xff);
            output.write(bytes.length & 0xff);
            output.writeBytes(bytes);
            return this;
        }

        Bytes rid(RoutingId value) {
            byte[] bytes = value.toBytes();
            output.write(bytes.length);
            output.writeBytes(bytes);
            return this;
        }

        byte[] toByteArray() {
            return output.toByteArray();
        }
    }

    private static SessionRelocationRoute commit() {
        return new SessionRelocationRoute(
            relocation(), coordinator(), RelocationRole.TARGET,
            actor(), session(), SessionRelocationRouteAction.COMMIT,
            11, 12, TARGET, 4, 29);
    }

    private static RelocationIdentity relocation() {
        return new RelocationIdentity(7, 9);
    }

    private static RelocationCoordinatorFence coordinator() {
        return new RelocationCoordinatorFence(
            "coordinator", 3, SOURCE, 2, "store-v17");
    }

    private static ActorIdentity actor() {
        return new ActorIdentity("actor-1", 5);
    }

    private static SessionOwnerFence session() {
        return new SessionOwnerFence(
            SOURCE, 2, "session-owner", 8, SESSION, 6);
    }
}
