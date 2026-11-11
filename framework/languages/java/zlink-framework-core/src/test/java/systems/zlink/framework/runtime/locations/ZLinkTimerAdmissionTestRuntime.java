package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.atomic.AtomicLong;

public final class ZLinkTimerAdmissionTestRuntime {
    private ZLinkTimerAdmissionTestRuntime() {}

    /**
     * Moves the recorded admission deadline to the current time and runs the owner-deadline task on
     * the state lane, as the scheduler would when the deadline arrives.
     */
    public static void advanceToDeadline(ZLinkLocationRuntime runtime) {
        runtime.stateLane
                .runAsync(
                        () -> {
                            runtime.ownerAdmissionDeadlineNanos = runtime.monotonicNow.getAsLong();
                            runtime.ownerDeadlineCore();
                            return null;
                        })
                .toCompletableFuture()
                .join();
    }

    /** Runs the owner-deadline task now, as the scheduler would at the recorded deadline. */
    public static void latch(ZLinkLocationRuntime runtime) {
        runtime.ownerDeadlineCore();
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
