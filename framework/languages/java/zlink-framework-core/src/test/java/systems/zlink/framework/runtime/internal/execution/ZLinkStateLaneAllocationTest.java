package systems.zlink.framework.runtime.internal.execution;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

/** Measures the completion and invocation storage of actual lane turns. */
public final class ZLinkStateLaneAllocationTest {
    private static final int TURNS = 32;
    private static final int WAIT_SECONDS = 5;
    private static final Object VALUE = new Object();
    private static final Supplier<Object> WORK = () -> VALUE;
    private static volatile Object observed;

    @Test
    void eachResultTurnUsesOneCompletionAndInvocationObject() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    public static void main(String[] args) throws Exception {
        for (int index = 0; index < 200; index++) idleTurns();
        queuedTurns(null);
        Path file = Files.createTempFile("zlink-state-lane-allocation-", ".jfr");
        try {
            Counts idle = record(file, ZLinkStateLaneAllocationTest::idleTurns);
            Counts queued = queuedTurns(file);
            Counts busy = busyTurns(file);
            Counts control =
                    record(
                            file,
                            () -> {
                                for (int index = 0; index < TURNS; index++)
                                    observed = new CompletableFuture<>();
                            });
            System.out.printf(
                    "turns=%d idleObjects=%d idleBytes=%d queuedObjects=%d queuedBytes=%d busyObjects=%d busyBytes=%d controlFutures=%d idle=%s queued=%s busy=%s%n",
                    TURNS,
                    idle.turnObjects,
                    idle.turnBytes,
                    queued.turnObjects,
                    queued.turnBytes,
                    busy.turnObjects,
                    busy.turnBytes,
                    control.controlFutures,
                    idle.types,
                    queued.types,
                    busy.types);
            assertEquals(TURNS, control.controlFutures, "JFR must observe control allocations");
            assertAll(
                    () ->
                            assertEquals(
                                    TURNS,
                                    idle.turnObjects,
                                    "one completion owns each idle invocation"),
                    () ->
                            assertEquals(
                                    TURNS,
                                    queued.turnObjects,
                                    "one completion owns each queued invocation"),
                    () ->
                            assertEquals(
                                    0,
                                    busy.turnObjects,
                                    "a refused immediate turn needs no completion"));
        } finally {
            Files.deleteIfExists(file);
        }
    }

    private static void idleTurns() {
        var lane = new ZLinkStateLane(Runnable::run);
        for (int index = 0; index < TURNS; index++) {
            var result = lane.runNowOrQueue(WORK).toCompletableFuture();
            assertTrue(result.isDone());
            assertSame(VALUE, result.join());
            observed = result;
        }
        assertTrue(lane.closeAsync().toCompletableFuture().isDone());
    }

    private static Counts busyTurns(Path file) throws Exception {
        var scheduled = new LinkedBlockingQueue<Runnable>();
        var lane = new ZLinkStateLane(scheduled::add);
        var hold = new CompletableFuture<Void>();
        assertTrue(lane.tryPost(() -> hold));
        scheduled.remove().run();
        Supplier<Object> refused =
                () -> {
                    throw new AssertionError("a refused turn must not execute");
                };
        try {
            return record(
                    file,
                    () -> {
                        for (int index = 0; index < TURNS; index++)
                            assertNull(lane.tryRunNow(refused));
                    });
        } finally {
            hold.complete(null);
            while (!scheduled.isEmpty()) scheduled.remove().run();
            assertTrue(lane.closeAsync().toCompletableFuture().isDone());
        }
    }

    private static Counts queuedTurns(Path file) throws Exception {
        var scheduled = new LinkedBlockingQueue<Runnable>();
        var lane = new ZLinkStateLane(scheduled::add);
        var hold = new CompletableFuture<Void>();
        assertTrue(lane.tryPost(() -> hold));
        scheduled.remove().run();
        var mailboxField = ZLinkStateLane.class.getDeclaredField("mailbox");
        mailboxField.setAccessible(true);
        var mailbox = (ConcurrentLinkedQueue<?>) mailboxField.get(lane);
        var start = new CountDownLatch(1);
        var ready = new CountDownLatch(TURNS);
        var returned = new CountDownLatch(TURNS);
        var failures = new ConcurrentLinkedQueue<Throwable>();
        var threads = new ArrayList<Thread>();
        for (int index = 0; index < TURNS; index++) {
            var thread =
                    Thread.ofPlatform()
                            .start(
                                    () -> {
                                        ready.countDown();
                                        try {
                                            assertTrue(start.await(WAIT_SECONDS, TimeUnit.SECONDS));
                                            assertSame(VALUE, lane.awaitTurn(WORK));
                                            assertFalse(lane.isOnLane());
                                        } catch (Throwable failure) {
                                            failures.add(failure);
                                        } finally {
                                            returned.countDown();
                                            scheduled.add(() -> {});
                                        }
                                    });
            threads.add(thread);
        }
        try {
            assertTrue(ready.await(WAIT_SECONDS, TimeUnit.SECONDS));
            Runnable publish =
                    () -> {
                        start.countDown();
                        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(WAIT_SECONDS);
                        while (mailbox.size() != TURNS && System.nanoTime() < deadline)
                            Thread.onSpinWait();
                        assertEquals(TURNS, mailbox.size());
                        assertEquals(TURNS, returned.getCount(), "queued callers still wait");
                    };
            if (file == null) {
                publish.run();
                return null;
            }
            return record(file, publish);
        } finally {
            start.countDown();
            hold.complete(null);
            while (returned.getCount() != 0) scheduled.take().run();
            Runnable remaining;
            while ((remaining = scheduled.poll()) != null) remaining.run();
            for (var thread : threads) thread.join();
            assertEquals(0, returned.getCount());
            assertTrue(failures.isEmpty(), failures.toString());
            assertTrue(lane.closeAsync().toCompletableFuture().isDone());
        }
    }

    private static Counts record(Path file, Runnable work) throws Exception {
        try (var recording = new Recording()) {
            recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
            recording.enable("jdk.DataLoss");
            recording.start();
            work.run();
            recording.stop();
            recording.dump(file);
        }
        long turns = 0, turnBytes = 0, controls = 0;
        Map<String, Long> types = new HashMap<>();
        try (var events = new RecordingFile(file)) {
            while (events.hasMoreEvents()) {
                var event = events.readEvent();
                assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                    continue;
                String allocated = event.getClass("objectClass").getName();
                if (!allocated.equals(CompletableFuture.class.getName())
                        && !allocated.startsWith(ZLinkStateLane.class.getName() + "$")) continue;
                var stack = event.getStackTrace();
                assertNotNull(stack);
                boolean turn = false, lane = false;
                for (var frame : stack.getFrames()) {
                    String owner = frame.getMethod().getType().getName();
                    lane |=
                            owner.equals(ZLinkStateLane.class.getName())
                                    || owner.startsWith(ZLinkStateLane.class.getName() + "$");
                    turn |=
                            owner.equals(ZLinkStateLane.class.getName())
                                    && (frame.getMethod().getName().equals("runNowOrQueue")
                                            || frame.getMethod().getName().equals("tryRunNow")
                                            || frame.getMethod().getName().equals("awaitTurn"));
                }
                if (turn) {
                    turns++;
                    turnBytes += event.getLong("allocationSize");
                    types.merge(allocated, 1L, Long::sum);
                } else if (!lane && allocated.equals(CompletableFuture.class.getName())) {
                    controls++;
                }
            }
        }
        return new Counts(turns, turnBytes, controls, types);
    }

    private record Counts(
            long turnObjects, long turnBytes, long controlFutures, Map<String, Long> types) {}
}
