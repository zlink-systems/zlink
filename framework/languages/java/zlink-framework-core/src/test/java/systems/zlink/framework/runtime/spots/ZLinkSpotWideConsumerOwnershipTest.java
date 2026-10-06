package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.lang.reflect.Field;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpotWideConsumerOwnershipTest {
    @ParameterizedTest
    @CsvSource({"SPOT_WIDE,false", "SPOT_WIDE,true", "PER_ACTOR,true"})
    void actorClaimRequiresItsExecutionGate(ZLinkUserSpotExecutionMode mode, boolean occupiedGate)
            throws Exception {
        var tasks = new LinkedBlockingQueue<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(tasks::add, ZLinkExecutionLanePolicy.spot());
        var serials = new ZLinkSpotSerialExecutor(spot, Runnable::run, tasks::add, mode, false);
        var first = serials.claimActorQueue("first").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var second =
                serials.claimActorQueue("second").toCompletableFuture().get(5, TimeUnit.SECONDS);
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var calls = new AtomicInteger();
        var occupied =
                spot.enqueue(
                        () -> {
                            entered.complete(null);
                            return release;
                        },
                        null);
        var initial = tasks.poll(5, TimeUnit.SECONDS);
        assertTrue(initial != null);
        initial.run();
        entered.get(5, TimeUnit.SECONDS);
        if (!occupiedGate) release.complete(null);
        Field active = ZLinkSerialExecutionQueue.class.getDeclaredField("active");
        active.setAccessible(true);
        try {
            var terminals = new java.util.ArrayList<CompletableFuture<Void>>();
            for (var actor : List.of(first, second)) {
                var admitted = new CompletableFuture<Void>();
                terminals.add(
                        serials.executeActor(
                                        actor,
                                        0,
                                        () -> {
                                            synchronized (actor.relocationLane()) {
                                                assertTrue(
                                                        activeRecord(active, actor.relocationLane())
                                                                != null);
                                            }
                                            calls.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        admitted)
                                .toCompletableFuture());
                admitted.get(5, TimeUnit.SECONDS);
            }
            // Run every consumer task the runtime published while the Spot turn is occupied.
            // The bounded poll also permits a passive mailbox to publish no consumer task.
            Runnable task;
            while ((task = tasks.poll(100, TimeUnit.MILLISECONDS)) != null) task.run();
            assertEquals(
                    occupiedGate && mode == ZLinkUserSpotExecutionMode.SPOT_WIDE ? 0 : 2,
                    calls.get());
            for (var actor : List.of(first, second)) {
                synchronized (actor.relocationLane()) {
                    assertNull(
                            active.get(actor.relocationLane()),
                            "Actor dequeue/claim occurred while another turn owned the Spot gate");
                }
            }
            release.complete(null);
            while (!CompletableFuture.allOf(terminals.toArray(CompletableFuture[]::new)).isDone()) {
                task = tasks.poll(5, TimeUnit.SECONDS);
                assertTrue(task != null);
                task.run();
            }
            CompletableFuture.allOf(terminals.toArray(CompletableFuture[]::new))
                    .get(5, TimeUnit.SECONDS);
            assertEquals(2, calls.get());
        } finally {
            release.complete(null);
            occupied.toCompletableFuture().get(5, TimeUnit.SECONDS);
            serials.close();
            Runnable task;
            while ((task = tasks.poll(100, TimeUnit.MILLISECONDS)) != null) task.run();
        }
    }

    private static Object activeRecord(Field field, ZLinkSerialExecutionQueue queue) {
        try {
            return field.get(queue);
        } catch (IllegalAccessException failure) {
            throw new AssertionError(failure);
        }
    }
}
