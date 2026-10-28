package systems.zlink.framework.runtime.channels;

import java.util.ArrayDeque;
import java.util.List;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.TimeUnit;

final class ZLinkManualDispatchExecutor extends AbstractExecutorService {
    private final ArrayDeque<Runnable> work = new ArrayDeque<>();
    private boolean shutdown;

    @Override
    public void execute(Runnable command) {
        work.addLast(command);
    }

    void drain() {
        while (!work.isEmpty()) work.removeFirst().run();
    }

    @Override
    public void shutdown() {
        shutdown = true;
    }

    @Override
    public List<Runnable> shutdownNow() {
        shutdown = true;
        var remaining = List.copyOf(work);
        work.clear();
        return remaining;
    }

    @Override
    public boolean isShutdown() {
        return shutdown;
    }

    @Override
    public boolean isTerminated() {
        return shutdown && work.isEmpty();
    }

    @Override
    public boolean awaitTermination(long timeout, TimeUnit unit) {
        return isTerminated();
    }
}
