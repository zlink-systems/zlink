package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.lang.reflect.Field;
import java.util.concurrent.CompletableFuture;

final class ZLinkSpotSerialExecutorAsyncTest {
    @Test
    void awaitAllLanesReturnsStageWithoutWaitingForBusyStateLane() throws Exception {
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.PER_ACTOR,
                        false);
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane owner = (ZLinkStateLane) laneField.get(serials);
        ZLinkStateLane caller = new ZLinkStateLane();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        assertTrue(
                owner.tryPost(
                        () -> {
                            entered.complete(null);
                            return release;
                        }));
        entered.join();
        try {
            var waiting =
                    caller.runAsync(
                                    () ->
                                            serials.awaitAllLanes(
                                                    ZLinkSerialExecutionQueue.Quiescence.ALL))
                            .thenCompose(stage -> stage);
            assertFalse(waiting.toCompletableFuture().isDone());
            release.complete(null);
            waiting.toCompletableFuture().join();
        } finally {
            release.complete(null);
            caller.closeAsync();
            serials.close();
        }
    }

    @Test
    void closeAsyncCompletesAfterTheStateLaneReleasesItsQueues() throws Exception {
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.PER_ACTOR,
                        false);
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane owner = (ZLinkStateLane) laneField.get(serials);
        ZLinkStateLane caller = new ZLinkStateLane();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        assertTrue(
                owner.tryPost(
                        () -> {
                            entered.complete(null);
                            return release;
                        }));
        entered.join();
        try {
            var closing = caller.runAsync(serials::closeAsync).thenCompose(stage -> stage);
            assertFalse(closing.toCompletableFuture().isDone());
            release.complete(null);
            closing.toCompletableFuture().join();
        } finally {
            release.complete(null);
            caller.closeAsync();
        }
    }
}
