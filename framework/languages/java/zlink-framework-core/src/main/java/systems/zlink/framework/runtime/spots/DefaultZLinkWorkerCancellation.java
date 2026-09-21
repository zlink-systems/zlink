package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.spots.ZLinkWorkerCancellation;

import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicBoolean;

final class DefaultZLinkWorkerCancellation implements ZLinkWorkerCancellation {
    private final AtomicBoolean cancellationRequested = new AtomicBoolean();

    void cancel() {
        cancellationRequested.set(true);
    }

    @Override
    public boolean isCancellationRequested() {
        return cancellationRequested.get();
    }

    @Override
    public void throwIfCancellationRequested() {
        if (isCancellationRequested()) {
            throw new CancellationException("worker call cancellation was requested");
        }
    }
}
