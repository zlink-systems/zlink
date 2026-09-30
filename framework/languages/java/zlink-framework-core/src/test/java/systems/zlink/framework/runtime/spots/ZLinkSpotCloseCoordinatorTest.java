package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.lang.reflect.InvocationTargetException;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkSpotCloseCoordinatorTest {
    @Test
    void successfulTerminalDetectsPendingCloseCommit() throws Exception {
        CompletableFuture<Boolean> commit = new CompletableFuture<>();
        var close = new ZLinkSpotCloseCoordinator(() -> commit, List.of(), ignored -> {});
        CompletableFuture<Boolean> result = close.close().toCompletableFuture();

        InvocationTargetException failure =
                assertThrows(InvocationTargetException.class, () -> forceSuccess(close, result));
        assertInstanceOf(AssertionError.class, failure.getCause());

        commit.complete(true);
        assertTrue(result.get(3, TimeUnit.SECONDS));
    }

    @Test
    void successfulTerminalDetectsPendingCloseStep() throws Exception {
        CompletableFuture<Void> step = new CompletableFuture<>();
        var close =
                new ZLinkSpotCloseCoordinator(
                        () -> CompletableFuture.completedFuture(true),
                        List.of(ZLinkSpotCloseCoordinator.Step.operation(() -> step)),
                        ignored -> {});
        CompletableFuture<Boolean> result = close.close().toCompletableFuture();

        InvocationTargetException failure =
                assertThrows(InvocationTargetException.class, () -> forceSuccess(close, result));
        assertInstanceOf(AssertionError.class, failure.getCause());

        step.complete(null);
        assertTrue(result.get(3, TimeUnit.SECONDS));
    }

    @Test
    void failedStepPreservesOriginalFailure() {
        IllegalStateException original = new IllegalStateException("close step failed");
        var close =
                new ZLinkSpotCloseCoordinator(
                        () -> CompletableFuture.completedFuture(true),
                        List.of(
                                ZLinkSpotCloseCoordinator.Step.operation(
                                        () -> CompletableFuture.failedFuture(original))),
                        ignored -> {});

        Throwable failure =
                assertThrows(
                                java.util.concurrent.CompletionException.class,
                                () -> close.close().toCompletableFuture().join())
                        .getCause();
        assertSame(original, failure);
    }

    private static void forceSuccess(
            ZLinkSpotCloseCoordinator close, CompletableFuture<Boolean> result) throws Exception {
        var end =
                ZLinkSpotCloseCoordinator.class.getDeclaredMethod(
                        "end", CompletableFuture.class, Boolean.class, Throwable.class);
        end.setAccessible(true);
        end.invoke(close, result, true, null);
    }
}
