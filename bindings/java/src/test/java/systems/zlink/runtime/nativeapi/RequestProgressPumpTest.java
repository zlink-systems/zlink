package systems.zlink.runtime.nativeapi;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.foreign.MemorySegment;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

/** Regression tests for progress-owner handoff ordering. */
public class RequestProgressPumpTest {
    @Test
    public void externalOwnerHandoffDoesNotHoldLockWhileJoiningPump()
        throws Exception {
        long key = 0x1234L;
        MemorySegment handle = MemorySegment.ofAddress(key);
        CountDownLatch pumpReady = new CountDownLatch(1);
        CountDownLatch allowTrack = new CountDownLatch(1);
        CountDownLatch trackReturned = new CountDownLatch(1);

        Thread pumpThread = new Thread(() -> {
            pumpReady.countDown();
            try {
                assertTrue(allowTrack.await(2, TimeUnit.SECONDS));
                RequestProgressPump.trackSocketRequest(
                    new CompletableFuture<>(), handle, "progress-pump-test");
                trackReturned.countDown();
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
            }
        }, "progress-pump-test");
        pumpThread.setDaemon(true);

        ConcurrentMap<Long, Object> pumps = map("PUMPS");
        ConcurrentMap<Long, CompletableFuture<Void>> stops = map("PUMP_STOPS");
        pumps.put(key, newPump(key, handle, pumpThread));
        Thread handoff = null;
        try {
            pumpThread.start();
            assertTrue(pumpReady.await(2, TimeUnit.SECONDS));

            handoff = new Thread(
                () -> RequestProgressPump.acquireExternalProgress(handle),
                "progress-owner-handoff-test");
            handoff.setDaemon(true);
            handoff.start();

            long deadline = System.nanoTime()
                + TimeUnit.SECONDS.toNanos(2);
            while (!stops.containsKey(key) && System.nanoTime() < deadline) {
                Thread.yield();
            }
            assertTrue(stops.containsKey(key),
                "handoff did not start stopping the fallback pump");

            allowTrack.countDown();
            assertTrue(trackReturned.await(2, TimeUnit.SECONDS),
                "pump callback could not acquire progress state during handoff");
            handoff.join(Duration.ofSeconds(2).toMillis());
            assertTrue(!handoff.isAlive(),
                "external progress handoff waited for a callback holding the lock");
        } finally {
            allowTrack.countDown();
            pumpThread.join(Duration.ofSeconds(2).toMillis());
            if (handoff == null || !handoff.isAlive()) {
                RequestProgressPump.releaseExternalProgress(handle);
            }
            pumps.remove(key);
            stops.remove(key);
        }
    }

    @SuppressWarnings("unchecked")
    private static <T> ConcurrentMap<Long, T> map(String fieldName)
        throws ReflectiveOperationException {
        Field field = RequestProgressPump.class.getDeclaredField(fieldName);
        field.setAccessible(true);
        return (ConcurrentMap<Long, T>) field.get(null);
    }

    private static Object newPump(long key, MemorySegment handle, Thread thread)
        throws ReflectiveOperationException {
        Class<?> pumpType = Class.forName(
            RequestProgressPump.class.getName() + "$Pump");
        Constructor<?> constructor = pumpType.getDeclaredConstructor(
            long.class, MemorySegment.class, String.class);
        constructor.setAccessible(true);
        Object pump = constructor.newInstance(key, handle, "progress-pump-test");
        Field threadField = pumpType.getDeclaredField("thread");
        threadField.setAccessible(true);
        threadField.set(pump, thread);
        return pump;
    }
}
