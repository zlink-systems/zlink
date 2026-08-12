package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.service
    .ZLinkServiceMessageFollowWireCodec;

final class ZLinkFrameworkActorSubsystemMessageFollowTest {
    @Test
    void noticeOriginIsTheCommittedSourceAndReceiverMayBeTheOriginalClient() {
        RoutingId source = RoutingId.from("source");
        RoutingId target = RoutingId.from("target");
        RoutingId client = RoutingId.from("client");
        var notice = new ZLinkServiceMessageFollowWireCodec.Notice(
            route("actor", source, 11, 13, 17),
            route("actor", target, 19, 23, 29),
            1,
            1,
            1,
            31,
            37,
            0);

        assertTrue(ZLinkFrameworkActorSubsystem.acceptsMessageFollowNotice(
            source, notice));
        assertFalse(ZLinkFrameworkActorSubsystem.acceptsMessageFollowNotice(
            target, notice));
        assertFalse(ZLinkFrameworkActorSubsystem.acceptsMessageFollowNotice(
            null, notice));
        assertFalse(ZLinkFrameworkActorSubsystem.acceptsMessageFollowNotice(
            source, null));
    }

    private static ZLinkServiceMessageFollowWireCodec.ActorRoute route(
        String actorId,
        RoutingId nodeRid,
        long nodeGeneration,
        long authorityGeneration,
        long leaseGeneration) {
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            actorId,
            7,
            nodeRid,
            nodeGeneration,
            authorityGeneration,
            leaseGeneration);
    }
}
