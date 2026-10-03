package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkServiceCompletionDispatcherFailureTest {
    @Test
    void failedCompletionIsReportedAndNextRegistrationRuns() throws Exception {
        var expected = new IllegalStateException("completion failed");
        var reported = new CompletableFuture<Throwable>();
        var later = new CompletableFuture<Void>();
        Logger logger = Logger.getLogger(ZLinkServiceCompletionDispatcher.class.getName());
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
            var dispatcher = ZLinkServiceCompletionDispatcher.INSTANCE;
            var first =
                    new ZLinkServiceCompletionDispatcher.WorkItem() {
                        void dispatch() {
                            throw expected;
                        }
                    };
            var next =
                    new ZLinkServiceCompletionDispatcher.WorkItem() {
                        void dispatch() {
                            later.complete(null);
                        }
                    };
            dispatcher.register(first);
            dispatcher.register(next);
            dispatcher.post(first);
            dispatcher.post(next);
            assertSame(expected, reported.get(2, TimeUnit.SECONDS));
            later.get(2, TimeUnit.SECONDS);
        } finally {
            logger.removeHandler(listener);
        }
    }
}
