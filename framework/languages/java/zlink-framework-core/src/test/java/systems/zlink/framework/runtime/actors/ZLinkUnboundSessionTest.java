package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

import java.util.concurrent.CompletionException;

/**
 * Spec 04-actor-model §8.1: an operation needing a bound session with no valid binding ends with
 * InvalidOperation, and the failure surfaces at the call's terminal like every other call failure —
 * not thrown by the accessor that creates the call.
 */
final class ZLinkUnboundSessionTest {
    private static ZLinkActorContextState state() {
        return new ZLinkActorContextState(
                new ZLinkBackendActorRef(RoutingId.from("node"), "p7", 1), "mesh", "entry");
    }

    @Test
    void anUnboundActorStillHandsOutASessionInsteadOfThrowing() {
        var context = state();

        var session = assertDoesNotThrow(context::boundSessionOrUnbound);

        assertNotNull(session);
        assertNotNull(session.send("payload"));

        //  The accessor used to call this helper, so an unbound Actor threw
        //  before the call object existed and CompletionStage.exceptionally
        //  never saw the failure. The helper keeps that shape for callers that
        //  genuinely require a binding; the Actor context no longer uses it.
        var thrown = assertThrows(ZLinkFrameworkException.class, context::requireBoundSession);
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, thrown.kind());
    }

    @Test
    void theSendTerminalCarriesInvalidOperation() {
        var session = state().boundSessionOrUnbound();

        var failure =
                assertThrows(
                        CompletionException.class,
                        () -> session.send("payload").submit().toCompletableFuture().join());

        var framework =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () -> {
                            throw failure.getCause();
                        });
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, framework.kind());
    }

    @Test
    void metadataKeepsTheCallUsableBeforeTheTerminal() {
        var session = state().boundSessionOrUnbound();

        var call = session.send("payload").metadata("trace-id", "t1");

        assertThrows(CompletionException.class, () -> call.submit().toCompletableFuture().join());
    }

    @Test
    void disconnectCarriesTheSameFailure() {
        var session = state().boundSessionOrUnbound();

        var failure =
                assertThrows(
                        CompletionException.class,
                        () -> session.disconnect().toCompletableFuture().join());

        assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                ((ZLinkFrameworkException) failure.getCause()).kind());
    }
}
