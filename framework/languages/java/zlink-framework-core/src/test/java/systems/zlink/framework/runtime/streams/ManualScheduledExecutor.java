package systems.zlink.framework.runtime.streams;

import java.util.Comparator;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.LongSupplier;

/** Schedules commands against a test clock and runs due commands on demand. */
public final class ManualScheduledExecutor extends ScheduledThreadPoolExecutor {
    private final LinkedBlockingQueue<ScheduledCommand> scheduledCommands =
            new LinkedBlockingQueue<>();

    record ScheduledCommand(
            Runnable command, long delayNanos, long dueNanos, ScheduledFuture<?> future) {}

    private final LongSupplier clock;

    ManualScheduledExecutor() {
        this(() -> 0);
    }

    public ManualScheduledExecutor(LongSupplier clock) {
        super(1);
        this.clock = clock;
        setRemoveOnCancelPolicy(true);
    }

    @Override
    public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
        ScheduledFuture<?> future = super.schedule(() -> {}, 1, TimeUnit.DAYS);
        long delayNanos = unit.toNanos(delay);
        scheduledCommands.add(
                new ScheduledCommand(command, delayNanos, clock.getAsLong() + delayNanos, future));
        return future;
    }

    public boolean hasTimerDueAt(long nanos) {
        return scheduledCommands.stream()
                .anyMatch(
                        command -> command.dueNanos() == nanos && !command.future().isCancelled());
    }

    public void runDueCommands() {
        scheduledCommands.removeIf(command -> command.future().isCancelled());
        for (; ; ) {
            ScheduledCommand due =
                    scheduledCommands.stream()
                            .filter(command -> command.dueNanos() - clock.getAsLong() <= 0)
                            .min(Comparator.comparingLong(ScheduledCommand::dueNanos))
                            .orElse(null);
            if (due == null) {
                return;
            }
            scheduledCommands.remove(due);
            if (!due.future().isCancelled()) {
                due.future().cancel(false);
                due.command().run();
            }
        }
    }

    ScheduledCommand takeScheduledCommand() throws InterruptedException {
        ScheduledCommand command = scheduledCommands.poll(5, TimeUnit.SECONDS);
        if (command == null) {
            throw new AssertionError("scheduled command was not registered");
        }
        return command;
    }
}
