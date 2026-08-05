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
        var execution = new ZLinkSuspendInvocationContext.ApplicationExecution(
            "room-1",
            "actor-a",
            true,
            true,
            actorId -> actorId.equals("actor-b"));

        try (var ignored =
                 ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            assertInvalid(() ->
                ZLinkSuspendInvocationContext.rejectSameSpotWait("room-1"));
            assertInvalid(() ->
                ZLinkSuspendInvocationContext.rejectSameActorWait("actor-a"));
            assertInvalid(() ->
                ZLinkSuspendInvocationContext.rejectSameActorWait("actor-b"));
            assertInvalid(() ->
                ZLinkSuspendInvocationContext.rejectCurrentActorJoinWait("actor-a"));
            assertDoesNotThrow(() ->
                ZLinkSuspendInvocationContext.rejectSameSpotWait("room-2"));
            assertDoesNotThrow(() ->
                ZLinkSuspendInvocationContext.rejectSameActorWait("actor-c"));
        }
    }

    @Test
    void perActorContextRejectsYieldButDoesNotApplySharedGateGuard() {
        var execution = new ZLinkSuspendInvocationContext.ApplicationExecution(
            "room-1",
            "actor-a",
            false,
            false,
            actorId -> true);

        try (var ignored =
                 ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            assertInvalid(() ->
                ZLinkSuspendInvocationContext.requireYieldAllowed("request"));
            assertDoesNotThrow(() ->
                ZLinkSuspendInvocationContext.rejectSameSpotWait("room-1"));
            assertDoesNotThrow(() ->
                ZLinkSuspendInvocationContext.rejectSameActorWait("actor-a"));
        }
    }

    private static void assertInvalid(Runnable operation) {
        ZLinkFrameworkException failure = assertThrows(
            ZLinkFrameworkException.class,
            operation::run);
        assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, failure.kind());
    }
}
