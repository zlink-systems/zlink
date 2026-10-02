package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkProcessExecutionLanesFailureTest {
    @Test
    void dispatchFailureIsReportedAndLaterWorkStillRuns() throws Exception {
        var expected = new IllegalStateException("dispatch failed");
        var reported = new CompletableFuture<Throwable>();
        Logger logger = Logger.getLogger(ZLinkProcessExecutionLanes.class.getName());
        Handler listener =
                new Handler() {
                    public void publish(LogRecord record) {
                        reported.complete(record.getThrown());
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(listener);
        try {
            var lane = ZLinkProcessExecutionLanes.applicationLane();
            var later = new CompletableFuture<Void>();
            lane.execute(
                    () -> {
                        throw expected;
                    });
            lane.execute(() -> later.complete(null));
            assertSame(expected, reported.get(2, TimeUnit.SECONDS));
            later.get(2, TimeUnit.SECONDS);
        } finally {
            logger.removeHandler(listener);
        }
    }
}
