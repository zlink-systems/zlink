package systems.zlink.framework.spots;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

/**
 * Fluent worker offload call created by a CPU or I/O worker operation.
 *
 * <p>{@link #submit()} keeps the current Spot turn. {@link #yield()} releases it
 * while the work is pending and resumes through the Spot queue.
 *
 * <p>Failures are projected as {@code ZLinkWorkerQueueFullException},
 * {@code ZLinkWorkerTimeoutException}, or {@code ZLinkWorkerFailedException}. A late
 * result arriving after a timeout is dropped without invoking user callbacks again.
 */
public interface ZLinkWorkerCall<T> {
    ZLinkWorkerCall<T> timeout(Duration timeout);

    CompletionStage<T> submit();

    CompletionStage<T> yield();

}
