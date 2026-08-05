package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

final class ZLinkActorContextStateRelocationTest {
    @Test
    void deferredJoinClaimBelongsToActorIncarnationRatherThanGlobalActorId() {
        var source = new ZLinkActorContextState(
            new ZLinkBackendActorRef(RoutingId.from("source"), "actor-a", 7),
            "mesh",
            "entry-source");
        var target = new ZLinkActorContextState(
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
    void snapshotsBoundSessionFenceWithoutAdvancingAcceptedSequence() {
        RoutingId actorNode = RoutingId.from("actor-node");
        RoutingId sessionOwner = RoutingId.from("session-owner");
        RoutingId session = RoutingId.from("session-a");
        var state = new ZLinkActorContextState(
            new ZLinkBackendActorRef(actorNode, "actor-a", 7),
            "mesh",
            "entry-a");

        long binding = state.bindSession(
            new TestBoundSession(), sessionOwner, session);
        var initial = state.boundSessionSourceSnapshot();
        assertEquals(binding, initial.bindingGeneration());
        assertEquals(0, initial.sessionSequence());

        assertEquals(1, state.nextBoundSessionSource().sessionSequence());
        var sealed = state.boundSessionSourceSnapshot();
        assertEquals(sessionOwner, sealed.sourceNodeRid());
        assertEquals(session, sealed.sourceSessionRid());
        assertEquals(binding, sealed.bindingGeneration());
        assertEquals(1, sealed.sessionSequence());
        assertEquals(1, state.boundSessionSourceSnapshot().sessionSequence());
    }

    @Test
    void failedMoveClearsMovingStateAndCompletesTheMoveStageExceptionally() {
        var state = new ZLinkActorContextState(
            new ZLinkBackendActorRef(RoutingId.from("source"), "actor-a", 7),
            "mesh",
            "entry-a");
        IllegalStateException failure =
            new IllegalStateException("join callback failed");

        state.beginMove();
        assertTrue(state.moving());

        state.failMove(failure);

        assertFalse(state.moving());
        CompletionException terminal = assertThrows(
            CompletionException.class,
            () -> state.moveCompletion().toCompletableFuture().join());
        assertEquals(failure, terminal.getCause());
    }

    private static final class TestBoundSession implements ZLinkBoundSession {
        @Override public ZLinkBoundSessionSendCall send(Object message) {
            throw new UnsupportedOperationException();
        }

        @Override public java.util.concurrent.CompletionStage<Void> disconnect() {
            return CompletableFuture.completedFuture(null);
        }
    }
}
