package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

final class ZLinkJavaRawMeshNodeDurableReplayTest {
    enum Operation { ACTOR_JOIN, ACTOR_CREATE, BOUND_SESSION_BIND }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void missingRawMeshRouteWaitsUntilDeadlineAndRemainsUnadmitted(Operation operation) {
        RoutingId sourceRid = RoutingId.from("durable-source");
        RoutingId targetRid = RoutingId.from("durable-missing-target");
        Duration timeout = Duration.ofMillis(80);
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://durable-no-route-" + System.nanoTime());
            source.start();
            var actor = new ZLinkBackendActorRef(targetRid, "actor", 3);
            var spots = (ZLinkJavaRawSpotNode) source.spotNode();
            spots.rememberActorAuthority(actor, 9, 10);
            spots.rememberSpotAuthority(targetRid, "spot", 7, 9, 10);
            long started = System.nanoTime();
            CompletionStage<?> completion = switch (operation) {
                case ACTOR_JOIN -> source.requestCanonicalActorJoin(
                    new ZLinkInternalMeshNode.CanonicalActorJoinRequest(
                        new ZLinkBackendActorRef(sourceRid, "actor", 3),
                        source.lifecycleGeneration(), 5, 6,
                        targetRid, 8, "spot", 7, 9, 10, false,
                        "join", "application/json", new byte[] {123, 125}), timeout);
                case ACTOR_CREATE -> source.requestActorCreate(targetRid,
                    new ZLinkInternalMeshNode.ActorCreateIntent("actor", "player",
                        new ZLinkServiceM6BWireCodec.ReservationFence(
                            "reservation", "version", 9, 10, targetRid, 8, "owner", 6, 1),
                        18, 19, System.currentTimeMillis() + timeout.toMillis()), timeout);
                case BOUND_SESSION_BIND -> source.bindRemoteStreamSession(
                    RoutingId.from("session"), actor, 9, 11, true, timeout);
            };
            var failure = assertThrows(CompletionException.class,
                () -> completion.toCompletableFuture().join());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertTrue(System.nanoTime() - started >= timeout.toNanos());
        }
    }
}
