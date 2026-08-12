package systems.zlink.framework.runtime.binding;
import java.util.Objects;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;

/** Process-wide infrastructure and application lanes shared by all topologies. */
final class ZLinkProcessExecutionLanes {
    private static final ScheduledExecutorService DEADLINES =
        Executors.newSingleThreadScheduledExecutor(Thread.ofVirtual()
            .name("zlink-jvm-service-deadline")
            .factory());
    private static final ExecutorService APPLICATION =
        Executors.newThreadPerTaskExecutor(Thread.ofVirtual()
            .name("zlink-jvm-application-dispatch-", 0)
            .factory());

    private ZLinkProcessExecutionLanes() {
    }

    static ScheduledExecutorService deadlines() {
        return DEADLINES;
    }

    static Executor applicationLane() {
        return new SerialLane(APPLICATION);
    }

    private static final class SerialLane implements Executor {
        private final Executor executor;
        private final ConcurrentLinkedQueue<Runnable> pending =
            new ConcurrentLinkedQueue<>();
        private final AtomicBoolean draining = new AtomicBoolean();

        private SerialLane(Executor executor) {
            this.executor = executor;
        }

        @Override
        public void execute(Runnable command) {
            pending.add(Objects.requireNonNull(command, "command"));
            schedule();
        }

        private void schedule() {
            if (draining.compareAndSet(false, true)) {
                executor.execute(this::drain);
            }
        }

        private void drain() {
            try {
                Runnable command;
                while ((command = pending.poll()) != null) {
                    try {
                        command.run();
                    } catch (RuntimeException ignored) {
                        // The submitted dispatch owns its terminal failure.
                    }
                }
            } finally {
                draining.set(false);
                if (!pending.isEmpty()) {
                    schedule();
                }
            }
        }
    }
}
