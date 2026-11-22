package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkSpotAdmissionRejectionTest {
    @Test
    void committedCloseRejectsOrdinaryRequestsAsClosing() throws Exception {
        var queue = new ZLinkSerialExecutionQueue();
        var committed = new AtomicBoolean();
        var entered = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        var close =
                queue.enqueueLifecycleTransition(
                        () -> {
                            committed.set(true);
                            entered.complete(null);
                            return ZLinkSerialExecutionQueue.yieldCurrent(release);
                        },
                        ignored -> CompletableFuture.completedFuture(null),
                        committed::get,
                        ignored -> false);
        try {
            entered.get(5, TimeUnit.SECONDS);
            var failure = refuseRequest(queue, false);
            assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failure.kind());
            assertEquals("SPOT is closing.", failure.getMessage());
        } finally {
            release.complete(null);
            close.toCompletableFuture().get(5, TimeUnit.SECONDS);
            queue.close();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void sealedAdmissionReportsWhyTheRequestIsRefused(boolean instanceIntent) {
        var queue = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        try {
            queue.sealClosingAdmission();
            var failure = refuseRequest(queue, instanceIntent);
            assertEquals(
                    instanceIntent
                            ? ZLinkFrameworkErrorKind.UNAVAILABLE
                            : ZLinkFrameworkErrorKind.NOT_FOUND,
                    failure.kind());
            assertEquals(
                    instanceIntent
                            ? "The Instance Spot incarnation released its authority."
                            : "SPOT is closing.",
                    failure.getMessage());
        } finally {
            queue.close();
        }
    }

    private static ZLinkFrameworkException refuseRequest(
            ZLinkSerialExecutionQueue queue, boolean instanceIntent) {
        var result =
                queue.enqueueMessage(
                        new Object(),
                        ignored -> instanceIntent,
                        0,
                        () -> CompletableFuture.completedFuture(null),
                        () -> {},
                        null);
        var failure =
                assertThrows(
                        CompletionException.class,
                        () -> result.toCompletableFuture().orTimeout(5, TimeUnit.SECONDS).join());
        return assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
    }
}
