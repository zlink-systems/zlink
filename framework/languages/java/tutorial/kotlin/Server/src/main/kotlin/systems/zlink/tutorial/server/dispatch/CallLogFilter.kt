package systems.zlink.tutorial.server.dispatch

import java.util.concurrent.CompletionStage
import org.slf4j.LoggerFactory
import systems.zlink.framework.ZLinkHandlerFilter
import systems.zlink.framework.ZLinkHandlerFilterContext
import systems.zlink.framework.ZLinkHandlerFilterNext

// --8<-- [start:filter-implementation]
// Runs around every handler this node receives, so the same logging is not
// repeated in each handler. Calling next.invoke() runs the handler; skipping it
// does not.
//
// There is no Kotlin filter interface. This is the Java ZLinkHandlerFilter, so
// the method is not a suspend fun: it takes and returns a CompletionStage, and
// the generic parameter belongs to the method, not to the class.
class CallLogFilter : ZLinkHandlerFilter {

    private val log = LoggerFactory.getLogger(CallLogFilter::class.java)

    override fun <T : Any?> invoke(
        context: ZLinkHandlerFilterContext,
        next: ZLinkHandlerFilterNext<T>,
    ): CompletionStage<T> {
        val startedAt = System.nanoTime()
        log.info("dispatch start: {}", context.packetName())

        // Everything in whenComplete runs on the way back out, so the filters
        // unwind in reverse registration order.
        return next.invoke().whenComplete { _, _ ->
            val elapsedMs = (System.nanoTime() - startedAt) / 1_000_000
            log.info("dispatch done: {} in {}ms", context.packetName(), elapsedMs)
        }
    }
}
// --8<-- [end:filter-implementation]
