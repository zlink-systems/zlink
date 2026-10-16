package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

import java.io.IOException;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkShutdownReasonTest {
    private static final long WAIT_SECONDS = 10;

    @Test
    void callbackFailureBeforeDeadlineRemainsTeardownFailed() throws Exception {
        assertCleanupReason("spot_closing", new IOException("callback failed"), false);
    }

    @Test
    void storeFailureBeforeDeadlineRemainsTeardownFailed() throws Exception {
        assertCleanupReason("descriptor_remove", new IOException("Store delete failed"), false);
    }

    @Test
    void callbackTimeoutIsAnActualFailure() throws Exception {
        assertCleanupReason("spot_closing", new TimeoutException("callback timeout"), false);
    }

    @Test
    void actualFailureSuppressedOnDeadlineRemainsTeardownFailed() throws Exception {
        assertCleanupReason("descriptor_remove", new IOException("Store delete failed"), true);
    }

    @Test
    void deadlineWithoutActualFailureReportsDeadlineExceeded() throws Exception {
        assertForcedReason(
                new systems.zlink.framework.runtime.internal.execution
                        .ZLinkTeardownDeadlineExceededException(),
                ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED);
    }

    @Test
    void successfulShutdownReportsStoppedNone() throws Exception {
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions())) {
            var result =
                    runtime.shutdown().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkTerminationOutcome.STOPPED, result.outcome());
            assertEquals(ZLinkFrameworkTerminationReason.NONE, result.reason());
        }
    }

    private void assertCleanupReason(String stage, Throwable failure, boolean deadlineFirst)
            throws Exception {
        var cleanup = new ZLinkFrameworkShutdown(Instant.EPOCH);
        // Reverse registration order is the runtime's cleanup order. Completed failed
        // operations remain observable even when the cleanup deadline has expired.
        if (deadlineFirst) {
            cleanup.deferStage(stage, () -> CompletableFuture.failedFuture(failure));
            cleanup.deferStage("pending_cleanup", CompletableFuture::new);
        } else {
            cleanup.deferStage("pending_cleanup", CompletableFuture::new);
            cleanup.deferStage(stage, () -> CompletableFuture.failedFuture(failure));
        }
        var observed =
                cleanup.closeAsync()
                        .handle((ignored, error) -> error)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
        assertForcedReason(observed, ZLinkFrameworkTerminationReason.TEARDOWN_FAILED);
    }

    private void assertForcedReason(Throwable failure, ZLinkFrameworkTerminationReason expected)
            throws Exception {
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions())) {
            // Inject the result of the actual cleanup chain at the runtime's force-stop
            // boundary, then observe its public result and published status.
            var force =
                    java.util.Arrays.stream(ZLinkFrameworkRuntime.class.getDeclaredMethods())
                            .filter(
                                    method ->
                                            method.getName().equals("forceStop")
                                                    && method.getParameterCount() == 2)
                            .findFirst()
                            .orElseThrow();
            force.setAccessible(true);
            var reasonType = force.getParameterTypes()[0];
            Object reason =
                    java.util.Arrays.stream(reasonType.getEnumConstants())
                            .filter(value -> value.toString().equals("DEADLINE_EXCEEDED"))
                            .findFirst()
                            .orElseThrow();
            force.invoke(runtime, reason, failure);
            var result =
                    runtime.shutdown().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, result.outcome());
            assertEquals(expected, result.reason());
            assertEquals(result, runtime.status().terminationResult().orElseThrow());
        }
    }
}
