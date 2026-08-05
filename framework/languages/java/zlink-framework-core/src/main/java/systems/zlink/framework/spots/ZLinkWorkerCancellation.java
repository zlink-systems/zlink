package systems.zlink.framework.spots;

/**
 * Cooperative cancellation signal supplied to CPU and I/O worker tasks.
 *
 * <p>The signal is raised when a worker call times out, its returned future is
 * cancelled, or the framework worker pool shuts down. A task that performs
 * interruptible or repeated work should check this signal and stop promptly.
 */
public interface ZLinkWorkerCancellation {
    boolean isCancellationRequested();

    void throwIfCancellationRequested();
}
