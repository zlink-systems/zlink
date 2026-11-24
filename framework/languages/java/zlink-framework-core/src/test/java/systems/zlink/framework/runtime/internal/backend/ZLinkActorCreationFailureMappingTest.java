package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.assertEquals;

import static systems.zlink.framework.runtime.protocol.ServiceWireConstants.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;

final class ZLinkActorCreationFailureMappingTest {
    @ParameterizedTest
    @CsvSource({
        "UNAVAILABLE, 107, 21",
        "SHUTTING_DOWN, 103, 0",
        "DEADLINE_EXCEEDED, 105, 19",
        "REJECTED, 106, 15",
        "PROTOCOL_ERROR, 104, 16",
        "INTERNAL_FAILURE, 105, 17"
    })
    void preservesTypedCause(ZLinkFrameworkErrorKind kind, int result, int code) {
        var cause = new ZLinkFrameworkException(kind, "creation failed");
        int[] terminal =
                ZLinkRequestFailureMapping.actorCreation(
                        new CompletionException(new ExecutionException(cause)));
        assertEquals(result, terminal[0]);
        assertEquals(code, terminal[1]);
        assertEquals(
                kind,
                ZLinkBackendRequestResult.fromWireTerminal(terminal[0])
                        .toFrameworkErrorKind(terminal[1]));
    }

    @Test
    void preservesExplicitRouteNotConnected() {
        var unavailable =
                ZLinkRequestFailureMapping.receivedFailure(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "no compatible Entry Spot",
                        (int) FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED,
                        Map.of());

        int[] terminal = ZLinkRequestFailureMapping.actorCreation(unavailable);
        assertEquals(FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED, terminal[1]);
    }

    @Test
    void untypedCallbackFailureUsesActorCreateFailed() {
        var cause = new IllegalStateException("callback failed");
        int[] terminal = ZLinkRequestFailureMapping.actorCreation(new CompletionException(cause));
        assertEquals(FRAMEWORK_ERROR_ACTOR_CREATE_FAILED, terminal[1]);
        assertEquals(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                ZLinkBackendRequestResult.fromWireTerminal(terminal[0])
                        .toFrameworkErrorKind(terminal[1]));
    }
}
