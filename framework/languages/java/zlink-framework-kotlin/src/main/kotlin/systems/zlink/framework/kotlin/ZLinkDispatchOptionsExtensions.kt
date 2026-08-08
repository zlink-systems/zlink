package systems.zlink.framework.kotlin

import systems.zlink.framework.configuration.ZLinkDispatchOptions
import systems.zlink.framework.configuration.ZLinkFrameworkOptions

/**
 * Configure dispatch/diagnostics as a Kotlin DSL block:
 * ```
 * options.configureDispatch {
 *     messageFlow(ZLinkMessageFlowLogMode.NORMAL)
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
