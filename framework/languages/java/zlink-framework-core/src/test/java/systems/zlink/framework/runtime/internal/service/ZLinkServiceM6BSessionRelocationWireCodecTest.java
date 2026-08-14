package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HexFormat;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.ActorIdentity;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.ActorRouteFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.RelocationCoordinatorFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.RelocationIdentity;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.RelocationRole;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionOwnerFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationRoute;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationRouteAction;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationSeal;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SessionRelocationSealed;

final class ZLinkServiceM6BSessionRelocationWireCodecTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId SESSION = RoutingId.from("session");

    private final ZLinkServiceM6BWireCodec codec =
        new ZLinkServiceM6BWireCodec();

    @Test
    void commands42And43MatchTheExactSharedShape() throws Exception {
        SessionRelocationSeal seal = seal();
        assertEquals(seal, codec.decodeSessionRelocationSeal(
            codec.encodeSessionRelocationSeal(seal)));
        assertArrayEquals(golden("sessionRelocationSeal"),
            codec.encodeSessionRelocationSeal(seal));

        SessionRelocationSealed sealed = new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(), seal.session());
        assertEquals(sealed, codec.decodeSessionRelocationSealed(
            codec.encodeSessionRelocationSealed(sealed)));
        assertArrayEquals(golden("sessionRelocationSealed"),
            codec.encodeSessionRelocationSealed(sealed));
        assertTrue(sealed.echoes(seal));
        assertFalse(new SessionRelocationSealed(
            seal.relocation(), seal.coordinator(), seal.actor(),
            new SessionOwnerFence(
                SOURCE, 2, "session-owner", 8, SESSION, 7)).echoes(seal));
    }

    @Test
    void command44CommitAndAbortMatchTheExactSharedShape() throws Exception {
        SessionRelocationRoute commit = commit();
        assertEquals(commit, codec.decodeSessionRelocationRoute(
            codec.encodeSessionRelocationRoute(commit)));
        assertArrayEquals(golden("sessionRelocationRouteCommit"),
            codec.encodeSessionRelocationRoute(commit));

        SessionRelocationRoute abort = abort();
        assertEquals(abort, codec.decodeSessionRelocationRoute(
            codec.encodeSessionRelocationRoute(abort)));
        assertArrayEquals(golden("sessionRelocationRouteAbort"),
            codec.encodeSessionRelocationRoute(abort));
    }

    @Test
    void removedSequenceAndCommand45ShapesAreRejected() {
        byte[] sealed = codec.encodeSessionRelocationSealed(
            new SessionRelocationSealed(
                relocation(), coordinator(), actorRoute(), session()));
        assertThrows(ZLinkServiceWireException.class,
            () -> codec.decodeSessionRelocationSealed(
                Arrays.copyOf(sealed, sealed.length + Long.BYTES)));

        byte[] retiredCommand45 = codec.encodeSessionRelocationRoute(commit());
        retiredCommand45[3] = 45;
        assertThrows(ZLinkServiceWireException.class,
            () -> codec.decodeSessionRelocationRoute(retiredCommand45));
    }

    @Test
    void exactRolesAndMonotonicCommitFenceAreRequired() {
        SessionRelocationRoute abort = abort();
        assertThrows(ZLinkServiceWireException.class,
            () -> new SessionRelocationRoute(
                abort.relocation(), abort.coordinator(), RelocationRole.TARGET,
                abort.actor(), abort.session(), abort.action(),
                0, 11, null, 0));
        assertThrows(ZLinkServiceWireException.class,
            () -> new SessionRelocationRoute(
                relocation(), coordinator(), RelocationRole.TARGET,
                actor(), session(), SessionRelocationRouteAction.COMMIT,
                11, 11, TARGET, 4));
        assertThrows(ZLinkServiceWireException.class,
            () -> new SessionRelocationSeal(
                relocation(), coordinator(), RelocationRole.TARGET,
                actorRoute(), session()));
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

    private static SessionRelocationRoute commit() {
        return new SessionRelocationRoute(
            relocation(), coordinator(), RelocationRole.TARGET,
            actor(), session(), SessionRelocationRouteAction.COMMIT,
            11, 12, TARGET, 4);
    }

    private static SessionRelocationRoute abort() {
        return new SessionRelocationRoute(
            relocation(), coordinator(), RelocationRole.SOURCE,
            actor(), session(), SessionRelocationRouteAction.ABORT,
            0, 11, null, 0);
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

    private static byte[] golden(String name) throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(
            Files.readString(sharedFixture()));
        for (JsonNode canonical : fixture.path("canonical")) {
            if (name.equals(canonical.path("name").asText())) {
                return HexFormat.of().parseHex(canonical.path("hex").asText());
            }
        }
        throw new AssertionError("missing shared canonical frame: " + name);
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/session-relocation-barrier-v1.json");
            if (Files.isRegularFile(candidate)) return candidate;
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared Session relocation barrier fixture was not found");
    }
}
