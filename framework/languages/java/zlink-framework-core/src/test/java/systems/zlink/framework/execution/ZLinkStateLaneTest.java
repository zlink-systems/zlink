package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class ZLinkStateLaneTest {
    @Test
    void runAsyncReturnsTheResultOfTheWork() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertEquals(42, lane.runAsync(() -> 42).toCompletableFuture().get(3, TimeUnit.SECONDS));

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void runAsyncSurfacesAFailureToItsOwnCaller() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        CompletionException error = assertThrows(
            CompletionException.class,
            () -> lane.runAsync(() -> {
                throw new IllegalStateException("boom");
            }).toCompletableFuture().join());

        assertEquals("boom", error.getCause().getMessage());
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void runAsyncKeepsServingAfterAWorkItemThrows() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertThrows(
            CompletionException.class,
            () -> lane.runAsync(() -> {
                throw new IllegalStateException();
            }).toCompletableFuture().join());

        assertEquals(7, lane.runAsync(() -> 7).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void concurrentCallersMutateUnsynchronizedStateWithoutLosingUpdates() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        Map<Integer, Integer> state = new HashMap<>();
        int callers = 32;
        int perCaller = 50;
        List<CompletableFuture<Void>> work = new ArrayList<>();

        for (int caller = 0; caller < callers; caller++) {
            int callerNumber = caller;
            work.add(CompletableFuture.runAsync(() -> {
                for (int item = 0; item < perCaller; item++) {
                    int key = callerNumber * perCaller + item;
                    lane.runAsync(() -> state.put(key, key)).toCompletableFuture().join();
                }
            }));
        }

        CompletableFuture.allOf(work.toArray(CompletableFuture[]::new)).get(10, TimeUnit.SECONDS);
        assertEquals(
            callers * perCaller,
            lane.runAsync(state::size).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void workItemsNeverOverlap() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        AtomicInteger inFlight = new AtomicInteger();
        AtomicBoolean observedOverlap = new AtomicBoolean();
        List<CompletableFuture<Void>> work = new ArrayList<>();

        for (int index = 0; index < 64; index++) {
            work.add(CompletableFuture.runAsync(() -> lane.runAsync(() -> {
                if (inFlight.incrementAndGet() != 1) {
                    observedOverlap.set(true);
                }
                for (int spin = 0; spin < 200; spin++) {
                    Thread.onSpinWait();
                }
                inFlight.decrementAndGet();
            }).toCompletableFuture().join()));
        }

        CompletableFuture.allOf(work.toArray(CompletableFuture[]::new)).get(10, TimeUnit.SECONDS);
        assertFalse(observedOverlap.get());
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void postsFromOneCallerRunInPostOrder() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        List<Integer> order = new ArrayList<>();

        for (int value = 0; value < 100; value++) {
            int posted = value;
            assertTrue(lane.tryPost(() -> {
                order.add(posted);
                return CompletableFuture.completedFuture(null);
            }));
        }

        assertEquals(
            List.of(java.util.stream.IntStream.range(0, 100).boxed().toArray(Integer[]::new)),
            lane.runAsync(() -> List.copyOf(order)).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void drainingMoreThanOneBatchStillRunsEveryItem() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        AtomicInteger count = new AtomicInteger();

        for (int index = 0; index < 250; index++) {
            assertTrue(lane.tryPost(() -> {
                count.incrementAndGet();
                return CompletableFuture.completedFuture(null);
            }));
        }

        assertEquals(250, lane.runAsync(count::get).toCompletableFuture().get(3, TimeUnit.SECONDS));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void reenteringTheSameLaneFailsInsteadOfHanging() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        IllegalStateException error = lane.runAsync(() -> assertThrows(
            IllegalStateException.class,
            () -> lane.runAsync(() -> 1))).toCompletableFuture().get(3, TimeUnit.SECONDS);

        assertTrue(error.getMessage().contains("already runs on the state lane"));
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void isOnLaneIsTrueOnlyInsideATurn() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        assertFalse(lane.isOnLane());
        assertTrue(lane.runAsync(lane::isOnLane).toCompletableFuture().get(3, TimeUnit.SECONDS));
        assertFalse(lane.isOnLane());
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void aDifferentLaneIsEnterableFromInsideATurn() throws Exception {
        ZLinkStateLane outer = new ZLinkStateLane();
        ZLinkStateLane inner = new ZLinkStateLane();

        assertEquals(
            5,
            outer.runAsync(() -> inner.runAsync(() -> 5).toCompletableFuture().join())
                .toCompletableFuture().get(3, TimeUnit.SECONDS));

        outer.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        inner.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }

    @Test
    void closeAsyncWaitsForQueuedWork() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        AtomicInteger completed = new AtomicInteger();

        for (int index = 0; index < 200; index++) {
            lane.tryPost(() -> {
                completed.incrementAndGet();
                return CompletableFuture.completedFuture(null);
            });
        }

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        assertEquals(200, completed.get());
    }

    @Test
    void runAsyncAfterCloseThrows() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);

        assertThrows(IllegalStateException.class, () -> lane.runAsync(() -> 1));
    }

    @Test
    void tryPostAfterCloseReportsRefusalInsteadOfThrowing() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);

        assertFalse(lane.tryPost(() -> CompletableFuture.completedFuture(null)));
    }

    @Test
    void closeAsyncIsIdempotent() throws Exception {
        ZLinkStateLane lane = new ZLinkStateLane();

        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
        lane.closeAsync().toCompletableFuture().get(3, TimeUnit.SECONDS);
    }
}
