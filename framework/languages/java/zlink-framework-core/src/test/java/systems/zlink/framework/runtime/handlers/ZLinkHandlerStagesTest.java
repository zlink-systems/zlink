package systems.zlink.framework.runtime.handlers;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

final class ZLinkHandlerStagesTest {
    @Test
    void completesEveryStageAndPropagatesOneAggregateFailure() {
        List<Integer> completed = new ArrayList<>();
        IllegalStateException first = new IllegalStateException("first cleanup failure");
        IllegalArgumentException second = new IllegalArgumentException("second cleanup failure");
        CompletionException terminal =
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkHandlerStages.completeAll(
                                                List.of(
                                                        () -> {
                                                            completed.add(1);
                                                            throw first;
                                                        },
                                                        () -> {
                                                            completed.add(2);
                                                            return CompletableFuture.failedFuture(
                                                                    second);
                                                        },
                                                        () -> {
                                                            completed.add(3);
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }))
                                        .toCompletableFuture()
                                        .join());
        assertEquals(List.of(1, 2, 3), completed);
        assertSame(first, terminal.getCause());
        assertArrayEquals(new Throwable[] {second}, first.getSuppressed());
    }

    @Test
    void completesSuccessfulStagesInOrder() {
        List<Integer> completed = new ArrayList<>();
        ZLinkHandlerStages.completeAll(
                        List.of(
                                () -> ZLinkHandlerStages.fromRunnable(() -> completed.add(1)),
                                () -> ZLinkHandlerStages.fromRunnable(() -> completed.add(2))))
                .toCompletableFuture()
                .join();
        assertEquals(List.of(1, 2), completed);
    }
}
