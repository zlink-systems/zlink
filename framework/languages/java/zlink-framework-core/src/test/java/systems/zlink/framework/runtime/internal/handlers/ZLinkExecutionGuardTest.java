package systems.zlink.framework.runtime.internal.handlers;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkExecutionGuardTest {
    @Test
    void sharedSpotGateRejectsSameGateWaitsBeforeSubmission() {
        var execution =
                new ZLinkSuspendInvocationContext.ApplicationExecution(
                        "room-1", "actor-a", true, true, actorId -> actorId.equals("actor-b"));

        try (var ignored = ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            assertInvalid(() -> ZLinkSuspendInvocationContext.validateSpotRequest("room-1", false));
            assertInvalid(
                    () -> ZLinkSuspendInvocationContext.validateActorRequest("actor-a", false));
            assertInvalid(
                    () -> ZLinkSuspendInvocationContext.validateActorRequest("actor-b", false));
            assertInvalid(
                    () -> ZLinkSuspendInvocationContext.rejectCurrentActorJoinWait("actor-a"));
            assertDoesNotThrow(
                    () -> ZLinkSuspendInvocationContext.validateSpotRequest("room-2", false));
            assertDoesNotThrow(
                    () -> ZLinkSuspendInvocationContext.validateActorRequest("actor-c", false));
        }
    }

    @Test
    void perActorContextRejectsSelfRequestWithoutSharedGate() {
        var execution =
                new ZLinkSuspendInvocationContext.ApplicationExecution(
                        "room-1", "actor-a", false, false, actorId -> true);

        try (var ignored = ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            assertInvalid(() -> ZLinkSuspendInvocationContext.requireYieldAllowed("request"));
            assertDoesNotThrow(
                    () -> ZLinkSuspendInvocationContext.validateSpotRequest("room-1", false));
            assertInvalid(
                    () -> ZLinkSuspendInvocationContext.validateActorRequest("actor-a", false));
            assertDoesNotThrow(
                    () -> ZLinkSuspendInvocationContext.validateActorRequest("actor-b", false));
        }
    }

    private static void assertInvalid(Runnable operation) {
        ZLinkFrameworkException failure =
                assertThrows(ZLinkFrameworkException.class, operation::run);
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, failure.kind());
    }
}
