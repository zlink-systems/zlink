package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.atomic.AtomicLong;

public final class ZLinkTimerAdmissionTestRuntime {
    private ZLinkTimerAdmissionTestRuntime() {}

    public static void advanceToDeadline(ZLinkLocationRuntime runtime) {
        try {
            var clock = ZLinkLocationRuntime.class.getDeclaredField("monotonicNow");
            var deadline =
                    ZLinkLocationRuntime.class.getDeclaredField("ownerAdmissionDeadlineNanos");
            var lane = ZLinkLocationRuntime.class.getDeclaredField("stateLane");
            clock.setAccessible(true);
            deadline.setAccessible(true);
            lane.setAccessible(true);
            ((ZLinkStateLane) lane.get(runtime))
                    .runAsync(
                            () -> {
                                try {
                                    long now = deadline.getLong(runtime);
                                    clock.set(runtime, (java.util.function.LongSupplier) () -> now);
                                    latch(runtime);
                                    return null;
                                } catch (IllegalAccessException failure) {
                                    throw new AssertionError(failure);
                                }
                            })
                    .toCompletableFuture()
                    .join();
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }

    public static void latch(ZLinkLocationRuntime runtime) {
        try {
            var method = ZLinkLocationRuntime.class.getDeclaredMethod("ownerDeadlineCore");
            method.setAccessible(true);
            method.invoke(runtime);
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }

    public static ZLinkLocationRuntime create(AtomicLong now, ScheduledExecutorService executor) {
        return new ZLinkLocationRuntime(
                ZLinkRegisteredLocationStores.fromUnified(new ZLinkInMemoryLocationStore()),
                "timer-owner",
                Duration.ofSeconds(30),
                Duration.ofSeconds(5),
                Duration.ofSeconds(3),
                Duration.ofSeconds(5),
                now::get,
                executor,
                new ZLinkStateLane(Runnable::run));
    }
}
