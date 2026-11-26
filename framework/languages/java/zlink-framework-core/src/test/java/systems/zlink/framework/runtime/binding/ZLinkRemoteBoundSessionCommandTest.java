package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;

import java.util.concurrent.TimeUnit;

final class ZLinkRemoteBoundSessionCommandTest {
    @Test
    void command36DeliversMultipartPushAndFencesToTheSessionOwner() throws Exception {
        try (var route = new ZLinkNativeSessionRouteFixture()) {
            var ready = route.routeReady();
            route.connect();
            ready.get(5, TimeUnit.SECONDS);

            var push = route.firstPush();
            assertEquals(RoutingId.from("target-node"), push.source());
            var fence = push.header().actor();
            assertEquals(new ZLinkBackendActorRef(push.source(), "actor", 1), fence.actor());
            assertEquals(push.sourceGeneration(), fence.targetNodeGeneration());
            assertEquals(17, fence.authorityOwnerGeneration());
            assertEquals(11, fence.ownerLeaseGeneration());
            assertEquals(23, push.header().expectedBindingGeneration());
            var parts = ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(push.payload());
            try {
                assertEquals(1, parts.size());
                assertEquals("first-push", parts.getFirst().toUtf8String());
            } finally {
                parts.forEach(Message::close);
            }
        }
    }
}
