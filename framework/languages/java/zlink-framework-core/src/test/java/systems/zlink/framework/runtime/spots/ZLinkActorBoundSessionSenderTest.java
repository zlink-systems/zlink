package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

final class ZLinkActorBoundSessionSenderTest {
    @Test
    void successfulTransportSubmissionCompletesWithoutRetry() throws Exception {
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("sendActorBoundSession")) {
                    submissions.incrementAndGet();
                    return true;
                }
                throw new UnsupportedOperationException(method.getName());
            });
        ZLinkActorBoundSessionSender sender = new ZLinkActorBoundSessionSender(
            Duration.ofSeconds(1),
            () -> false,
            ignored -> { });

        sender.send(
                node,
                new ZLinkBackendActorRef(
                    RoutingId.from("actor-node"),
                    "actor-1",
                    1),
                "actor-1",
                new byte[] {1, 2, 3},
                "bound reply failed")
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS);

        assertEquals(1, submissions.get());
    }
}
