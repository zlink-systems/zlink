package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
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
 * Spec 15 §4.2 steps 7-8 and spec 20 §5: the target completes the Join and
 * opens dispatch before submitting one-way command 44. A failed submission is
 * diagnostic-only because the Session seal timeout owns the recovery.
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
    void failedCommand44SubmissionDoesNotFailTheCommittedJoin()
        throws Exception {
        IllegalStateException transportDown =
            new IllegalStateException("Session owner is unreachable");
        List<String> calls = new ArrayList<>();
        ZLinkActorSpotAdmission admission = admission(
            CompletableFuture.failedFuture(transportDown), calls);

        String outcome = admission.submitBoundSessionRouteAfterJoin(
                CompletableFuture.completedFuture("joined")
                    .thenApply(join -> {
                        calls.add("join-terminal");
                        return join;
                    }),
                request(),
                spotNode(targetActor()),
                routeUpdate())
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS);

        assertEquals("joined", outcome);
        assertEquals(List.of("join-terminal", "send-route"), calls,
            "command 44 submission must start only after the Join terminal");
    }

    @Test
    void supersededRouteSwitchIsReportedWithoutChangingTheJoinTerminal()
        throws Exception {
        ZLinkActorSpotAdmission admission = admission(
            CompletableFuture.completedFuture(null));

        //  actorLookup answers null: this node no longer holds the exact
        //  incarnation, so the announced relocation is over (spec 20 §5.1).
        CompletionStage<Void> outcome = admission.startBoundSessionRouteUpdate(
            request(),
            spotNode(null),
            routeUpdate());

        assertNull(outcome.toCompletableFuture().get(2, TimeUnit.SECONDS));
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
        return admission(routeSendOutcome, null);
    }

    private ZLinkActorSpotAdmission admission(
        CompletionStage<Void> routeSendOutcome,
        List<String> calls) {
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
                        ? sent(calls, routeSendOutcome)
                        : defaultValue(method.getReturnType()));
        ZLinkActorSpotAdmission admission = new ZLinkActorSpotAdmission();
        admission.attach(
            runtime,
            () -> false,
            new ZLinkSessionRelocationPeerClient(meshNode));
        return admission;
    }

    private static CompletionStage<Void> sent(
        List<String> calls,
        CompletionStage<Void> outcome) {
        if (calls != null) {
            calls.add("send-route");
        }
        return outcome;
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
