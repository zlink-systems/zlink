package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.util.Arrays;
import java.util.HexFormat;
import java.nio.file.Files;
import java.nio.file.Path;
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
    void command44AbortRequiresTheSourceRoleAndRoundTrips() {
        SessionRelocationRoute abort = abort();

        assertEquals(abort, codec.decodeSessionRelocationRoute(
            codec.encodeSessionRelocationRoute(abort)));
        assertThrows(ZLinkServiceWireException.class,
            () -> new SessionRelocationRoute(
                abort.relocation(), abort.coordinator(), RelocationRole.TARGET,
                abort.actor(), abort.session(), abort.action(),
                0, 11, null, 0, 0));
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

    @Test
    void command42And43MatchTheSharedGoldenFixture() throws Exception {
        SessionRelocationSeal seal = seal();
        assertArrayEquals(golden("sessionRelocationSeal"),
            codec.encodeSessionRelocationSeal(seal));

        SessionRelocationSealed sealed = new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(),
            seal.session(), 41);
        assertArrayEquals(golden("sessionRelocationSealed"),
            codec.encodeSessionRelocationSealed(sealed));
    }

    @Test
    void command44And45MatchTheSharedGoldenFixture() throws Exception {
        SessionRelocationRoute commit = commitWithGoldenHighWater();
        assertArrayEquals(golden("sessionRelocationRouteCommit"),
            codec.encodeSessionRelocationRoute(commit));
        assertArrayEquals(golden("sessionRelocationRoutedCommit"),
            codec.encodeSessionRelocationRouted(new SessionRelocationRouted(
                commit.relocation(), commit.coordinator(), commit.actor(),
                commit.session(), commit.action(),
                ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
                commit.currentAuthorityOwnerGeneration(),
                commit.lastAcceptedSessionSequence())));

        SessionRelocationRoute abort = abort();
        assertArrayEquals(golden("sessionRelocationRouteAbort"),
            codec.encodeSessionRelocationRoute(abort));
        assertArrayEquals(golden("sessionRelocationRoutedAbort"),
            codec.encodeSessionRelocationRouted(new SessionRelocationRouted(
                abort.relocation(), abort.coordinator(), abort.actor(),
                abort.session(), abort.action(),
                ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
                abort.currentAuthorityOwnerGeneration(), 41)));
    }

    private static SessionRelocationSeal seal() {
        return new SessionRelocationSeal(
            relocation(), coordinator(), RelocationRole.SOURCE,
            actorRoute(), session());
    }

    private static ActorRouteFence actorRoute() {
        return new ActorRouteFence(
            new ZLinkBackendActorRef(SOURCE, "actor-1", 5), 2, 11, 13);
    }

    private static byte[] golden(String name) throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(
            Files.readString(sharedFixture()));
        for (JsonNode canonical : fixture.path("canonical")) {
            if (name.equals(canonical.path("name").asText())) {
                return HexFormat.of().parseHex(
                    canonical.path("hex").asText());
            }
        }
        throw new AssertionError("missing shared canonical frame: " + name);
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/session-relocation-barrier-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared Session relocation barrier fixture was not found");
    }

    private static SessionRelocationRoute commit() {
        return new SessionRelocationRoute(
            relocation(), coordinator(), RelocationRole.TARGET,
            actor(), session(), SessionRelocationRouteAction.COMMIT,
            11, 12, TARGET, 4, 29);
    }

    private static SessionRelocationRoute commitWithGoldenHighWater() {
        return new SessionRelocationRoute(
            relocation(), coordinator(), RelocationRole.TARGET,
            actor(), session(), SessionRelocationRouteAction.COMMIT,
            11, 12, TARGET, 4, 41);
    }

    private static SessionRelocationRoute abort() {
        return new SessionRelocationRoute(
            relocation(), coordinator(), RelocationRole.SOURCE,
            actor(), session(), SessionRelocationRouteAction.ABORT,
            0, 11, null, 0, 0);
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
