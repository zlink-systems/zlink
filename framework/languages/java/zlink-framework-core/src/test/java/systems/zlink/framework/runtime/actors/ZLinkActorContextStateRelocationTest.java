package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode.BoundSessionRoute;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

final class ZLinkActorContextStateRelocationTest {
    @Test
    void directSessionAttachmentDoesNotInventASpotNodeBinding() {
        var state =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(RoutingId.from("actor-node"), "actor-a", 7),
                        "mesh",
                        "entry");
        long token =
                state.bindSession(
                        new ZLinkBoundSession() {
                            @Override
                            public ZLinkBoundSessionSendCall send(Object message) {
                                throw new UnsupportedOperationException();
                            }

                            @Override
                            public CompletionStage<Void> disconnect() {
                                return CompletableFuture.completedFuture(null);
                            }
                        });
        assertNull(state.boundSessionRoute());
        assertTrue(state.clearBoundSession(token));
    }

    @Test
    void bindingFactsComeFromTheSpotNodeRecord() {
        var ref = new ZLinkBackendActorRef(RoutingId.from("actor-node"), "actor-a", 7);
        var route =
                new AtomicReference<>(
                        new BoundSessionRoute(
                                RoutingId.from("owner"),
                                13,
                                "owner-id",
                                17,
                                RoutingId.from("session"),
                                19));
        var node = bindingNode(route::get);
        var state = new ZLinkActorContextState(ref, "mesh", "entry");
        var bound =
                new ZLinkNativeBoundSessionRuntime(
                        node,
                        ref,
                        null,
                        null,
                        null,
                        route.get().sessionOwnerNodeRid(),
                        route.get().sessionRid(),
                        Duration.ofSeconds(1),
                        null,
                        null);
        state.bindSession(bound);
        route.set(
                new BoundSessionRoute(
                        RoutingId.from("new-owner"),
                        23,
                        "new-owner-id",
                        29,
                        RoutingId.from("new-session"),
                        31));
        var current = state.boundSessionRoute();
        assertEquals(route.get().sessionOwnerNodeRid(), current.sessionOwnerNodeRid());
        assertEquals(23, current.sessionOwnerNodeGeneration());
        assertEquals(31, current.bindingGeneration());
        route.set(null);
        assertNull(state.boundSessionRoute());
    }

    @Test
    void deferredJoinClaimBelongsToActorIncarnationRatherThanGlobalActorId() {
        var source =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(RoutingId.from("source"), "actor-a", 7),
                        "mesh",
                        "entry-source");
        var target =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(RoutingId.from("target"), "actor-a", 8),
                        "mesh",
                        "entry-target");
        Object first = new Object();
        Object second = new Object();
        Object replacement = new Object();

        assertTrue(source.tryClaimDeferredJoin(first));
        assertFalse(source.tryClaimDeferredJoin(second));
        assertTrue(target.tryClaimDeferredJoin(second));
        source.releaseDeferredJoin(second);
        assertFalse(source.tryClaimDeferredJoin(replacement));
        source.releaseDeferredJoin(first);
        assertTrue(source.tryClaimDeferredJoin(replacement));

        source.releaseDeferredJoin(replacement);
        target.releaseDeferredJoin(second);
    }

    @Test
    void snapshotsBoundSessionFence() {
        RoutingId actorNode = RoutingId.from("actor-node");
        RoutingId sessionOwner = RoutingId.from("session-owner");
        RoutingId session = RoutingId.from("session-a");
        var state =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(actorNode, "actor-a", 7), "mesh", "entry-a");

        long binding = bindWithRecord(state, sessionOwner, session);
        var initial = state.boundSessionRoute();
        assertEquals(binding, initial.bindingGeneration());

        bindWithRecord(state, sessionOwner, session, binding);
        var sealed = state.boundSessionRoute();
        assertEquals(sessionOwner, sealed.sessionOwnerNodeRid());
        assertEquals(session, sealed.sessionRid());
        assertEquals(binding, sealed.bindingGeneration());
    }

    @Test
    void importsSessionOwnerFenceAcrossTargetBindingRecreation() {
        RoutingId actorNode = RoutingId.from("actor-node");
        RoutingId sessionOwner = RoutingId.from("session-owner");
        RoutingId session = RoutingId.from("session-a");
        var state =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(actorNode, "actor-a", 7), "mesh", "entry-a");

        bindWithRecord(state, sessionOwner, session);
        long localBindingToken = bindWithRecord(state, sessionOwner, session, 41);

        var imported = state.boundSessionRoute();
        assertEquals(41, imported.bindingGeneration());
        assertNotEquals(41, localBindingToken);
        assertTrue(state.clearBoundSession(localBindingToken));
    }

    @Test
    void cleanupRetiresOnlyTheCurrentBindingIdentityOnce() {
        RoutingId node = RoutingId.from("actor-node");
        RoutingId firstSession = RoutingId.from("session-a");
        RoutingId secondSession = RoutingId.from("session-b");
        var state =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(node, "actor-a", 7), "mesh", "entry-a");
        long first = bindWithRecord(state, node, firstSession);
        long second = bindWithRecord(state, node, secondSession);
        assertFalse(state.clearBoundSession(first));
        assertEquals(secondSession, state.boundSessionRoute().sessionRid());
        assertTrue(state.clearBoundSession(second));
        assertFalse(state.clearBoundSession(second));
    }

    @Test
    void failedMoveClearsMovingStateAndCompletesTheMoveStageExceptionally() {
        var state =
                new ZLinkActorContextState(
                        new ZLinkBackendActorRef(RoutingId.from("source"), "actor-a", 7),
                        "mesh",
                        "entry-a");
        IllegalStateException failure = new IllegalStateException("join callback failed");

        state.beginMove();
        assertTrue(state.moving());

        state.failMove(failure);

        assertFalse(state.moving());
        CompletionException terminal =
                assertThrows(
                        CompletionException.class,
                        () -> state.moveCompletion().toCompletableFuture().join());
        assertEquals(failure, terminal.getCause());
    }

    private static long bindWithRecord(
            ZLinkActorContextState state, RoutingId owner, RoutingId session) {
        return bindWithRecord(state, owner, session, state.sessionBindingToken() + 1);
    }

    private static long bindWithRecord(
            ZLinkActorContextState state, RoutingId owner, RoutingId session, long generation) {
        var route = new BoundSessionRoute(owner, 11, "owner-id", 13, session, generation);
        var node = bindingNode(() -> route);
        return state.bindSession(
                new ZLinkNativeBoundSessionRuntime(
                        node,
                        state.actorRef(),
                        null,
                        null,
                        null,
                        owner,
                        session,
                        Duration.ofSeconds(1),
                        null,
                        null));
    }

    private static ZLinkInternalSpotNode bindingNode(Supplier<BoundSessionRoute> route) {
        return (ZLinkInternalSpotNode)
                Proxy.newProxyInstance(
                        ZLinkActorContextStateRelocationTest.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalSpotNode.class},
                        (proxy, method, args) -> {
                            if (method.getName().equals("boundSessionRoute"))
                                return Optional.ofNullable(route.get());
                            throw new UnsupportedOperationException(method.getName());
                        });
    }
}
