package systems.zlink.framework.kotlin

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.configuration.ZLinkDispatchOptions
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver

/**
 * Configure dispatch/diagnostics as a Kotlin DSL block:
 * ```
 * options.configureDispatch {
 *     messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
 *     traceLogFile("logs/flow.log")
 *     traceLabel("api")
 * }
 * ```
 * The underlying tracing runtime is the shared Java core, so this is purely ergonomic.
 */
inline fun ZLinkFrameworkOptions.configureDispatch(
    block: ZLinkDispatchOptions.() -> Unit,
): ZLinkDispatchOptions {
    val dispatch = configureDispatch()
    dispatch.block()
    return dispatch
}

/** Register a message-flow observer from a Kotlin lambda (fire-and-forget). */
fun ZLinkDispatchOptions.onMessageFlow(
    observer: (ZLinkMessageFlowEvent) -> Unit,
): ZLinkDispatchOptions =
    setMessageFlowObserver(
        ZLinkMessageFlowObserver { event ->
            observer(event)
            CompletableFuture.completedFuture<Void>(null) as CompletionStage<Void>
        },
    )
