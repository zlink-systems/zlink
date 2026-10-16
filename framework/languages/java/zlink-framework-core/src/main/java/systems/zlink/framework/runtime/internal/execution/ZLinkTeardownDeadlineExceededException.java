package systems.zlink.framework.runtime.internal.execution;

import java.util.concurrent.TimeoutException;

/** Marks a Framework teardown wait that ended at the host's cleanup deadline. */
public final class ZLinkTeardownDeadlineExceededException extends TimeoutException {
    public ZLinkTeardownDeadlineExceededException() {
        this("host teardown deadline expired");
    }

    public ZLinkTeardownDeadlineExceededException(String message) {
        super(message);
    }
}
