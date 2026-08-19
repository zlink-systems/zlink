package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

/**
 * Spec 20 §5 step 5 / spec 52 §5: the target runtime submits command 44 as
 * part of its post-commit completion sequence. dotnet awaits the session
 * route commit inside the join completion and cpp fails the join route
 * reply when the route activation fails, so the Java direct-Join target
 * must fail the join attempt when the command-44 submission fails instead
 * of accepting a join whose cross-node pushes would silently drop.
 */
final class ZLinkActorSpotAdmissionSessionRouteTest {
    private static final RoutingId TARGET_NODE = RoutingId.from("target-node");
    private static final RoutingId SESSION_NODE = RoutingId.from("session-node");
    private static final String ACTOR_ID = "actor-route";

    private ZLinkActorRuntime runtime;

    @AfterEach
    void closeRuntime() {
        if (runtime != null) {
            runtime.closeAsync().toCompletableFuture().join();
        }
    }

    @Test
    void failedCommand44SubmissionFailsTheJoinRouteSwitchStage()
        throws Exception {
        IllegalStateException transportDown =
            new IllegalStateException("Session owner is unreachable");
        ZLinkActorSpotAdmission admission = admission(
            CompletableFuture.failedFuture(transportDown));

        CompletionStage<Void> outcome = admission.startBoundSessionRouteUpdate(
            request(),
            spotNode(targetActor()),
            routeUpdate());

        CompletableFuture<Void> settled = outcome.toCompletableFuture();
        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> {
                try {
                    settled.get(2, TimeUnit.SECONDS);
                } catch (java.util.concurrent.ExecutionException wrapped) {
                    throw new CompletionException(wrapped.getCause());
                }
            });
        assertSame(transportDown, failure.getCause(),
            "a failed command-44 submission must fail the join attempt "
                + "instead of being swallowed as a warning");
    }

    @Test
    void supersededRouteSwitchFailsTheJoinAttemptTyped() {
        ZLinkActorSpotAdmission admission = admission(
            CompletableFuture.completedFuture(null));

        //  actorLookup answers null: this node no longer holds the exact
        //  incarnation, so the announced relocation is over (spec 20 §5.1).
        CompletionStage<Void> outcome = admission.startBoundSessionRouteUpdate(
            request(),
            spotNode(null),
            routeUpdate());

        CompletableFuture<Void> settled = outcome.toCompletableFuture();
        assertTrue(settled.isCompletedExceptionally(),
            "a superseded route switch must fail the join attempt");
        CompletionException failure = assertThrows(
            CompletionException.class, settled::join);
        assertInstanceOf(ZLinkConfigurationException.class, failure.getCause());
    }

    @Test
    void acceptedCommand44SubmissionAndAbsentRouteCompleteTheStage()
        throws Exception {
        ZLinkActorSpotAdmission admission = admission(
            CompletableFuture.completedFuture(null));

        assertNull(admission.startBoundSessionRouteUpdate(
                request(),
                spotNode(targetActor()),
                routeUpdate())
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS));
        assertNull(admission.startBoundSessionRouteUpdate(
                request(),
                spotNode(targetActor()),
                null)
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS));
    }

    private ZLinkActorSpotAdmission admission(
        CompletionStage<Void> routeSendOutcome) {
        runtime = new ZLinkActorRuntime(
            spotNode(targetActor()),
            Map.of(),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer());
        ZLinkInternalMeshNode meshNode = (ZLinkInternalMeshNode)
            Proxy.newProxyInstance(
                ZLinkInternalMeshNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalMeshNode.class},
                (proxy, method, arguments) ->
                    "sendSessionRelocationRoute".equals(method.getName())
                        ? routeSendOutcome
                        : defaultValue(method.getReturnType()));
        ZLinkActorSpotAdmission admission = new ZLinkActorSpotAdmission();
        admission.attach(
            runtime,
            () -> false,
            new ZLinkSessionRelocationPeerClient(meshNode));
        return admission;
    }

    private static ZLinkInternalSpotNode spotNode(
        ZLinkBackendActorRef lookupAnswer) {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if ("actorLookup".equals(method.getName())) {
                    return lookupAnswer;
                }
                if ("routingId".equals(method.getName())) {
                    return TARGET_NODE;
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static Object defaultValue(Class<?> type) {
        if (type == boolean.class) {
            return false;
        }
        if (type == int.class) {
            return 0;
        }
        if (type == long.class) {
            return 0L;
        }
        if (type == double.class) {
            return 0.0d;
        }
        if (type == float.class) {
            return 0.0f;
        }
        if (type == short.class) {
            return (short) 0;
        }
        if (type == byte.class) {
            return (byte) 0;
        }
        if (type == char.class) {
            return (char) 0;
        }
        return null;
    }

    private static ZLinkBackendActorRef targetActor() {
        return new ZLinkBackendActorRef(TARGET_NODE, ACTOR_ID, 1L);
    }

    private static ZLinkActorSpotAdmission.BoundSessionRouteUpdate
        routeUpdate() {
        var command = new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(0x11L, 0x22L),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "owner-1", 1L, TARGET_NODE, 1L, "v1"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity(ACTOR_ID, 1L),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                SESSION_NODE, 1L, "session-owner", 1L,
                RoutingId.from("session-a"), 1L),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            1L,
            2L,
            TARGET_NODE,
            1L);
        return new ZLinkActorSpotAdmission.BoundSessionRouteUpdate(
            command, targetActor());
    }

    private static ZLinkActorSpotRoutePackets.TransferRequest request() {
        return new ZLinkActorSpotRoutePackets.TransferRequest(
            "commit",
            "00000000-0000-0011-0000-000000000022",
            1_000L,
            ACTOR_ID,
            "player",
            TARGET_NODE,
            1L,
            null,
            null,
            null,
            null,
            null,
            null,
            0,
            false,
            0L,
            0L,
            0L,
            0L,
            0L,
            0L,
            null);
    }
}
