package systems.zlink.tutorial.server.dispatch;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;

import java.util.concurrent.CompletionStage;

// Runs around every handler this node receives, so the same logging is not
// repeated in each handler. Invoking next runs the handler; skipping it does
// not.
// --8<-- [start:filter-implementation]
public final class CallLogFilter implements ZLinkHandlerFilter {

    private static final Logger LOG = LoggerFactory.getLogger(CallLogFilter.class);

    @Override
    public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
        long startedAt = System.nanoTime();
        LOG.info("dispatch start: {}", context.packetName());

        return next.invoke()
                .whenComplete(
                        (reply, failure) -> {
                            long elapsedMillis = (System.nanoTime() - startedAt) / 1_000_000L;
                            LOG.info(
                                    "dispatch done: {} in {}ms",
                                    context.packetName(),
                                    elapsedMillis);
                        });
    }
}
// --8<-- [end:filter-implementation]
