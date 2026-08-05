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
            route.currentAuthorityOwnerGeneration(),
            route.lastAcceptedSessionSequence());

        assertEquals(ack, codec.decodeSessionRelocationRouted(
            codec.encodeSessionRelocationRouted(ack)));
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
