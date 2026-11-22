package systems.zlink.framework.kotlin

import kotlin.coroutines.AbstractCoroutineContextElement
import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.ThreadContextElement
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext

internal object ZLinkCoroutineInvocationContext {
    fun capture(dispatcher: CoroutineDispatcher): CoroutineContext =
        dispatcher + InvocationContextElement()
}

// Private top-level declarations compile to package-private JVM classes, so this file adds
// nothing to the public JVM surface beyond ZLinkCoroutineInvocationContext.capture.
private object InvocationContextKey : CoroutineContext.Key<InvocationContextElement>

private class InvocationState(
    private val invocation: ZLinkSuspendInvocationContext.Snapshot,
    private val flow: ZLinkFlowContext.State?,
) {
    fun restore() {
        ZLinkSuspendInvocationContext.restore(invocation)
        val local = ZLinkFlowContext.threadLocal()
        if (flow == null) local.remove() else local.set(flow)
    }
}

/** One element carries every invocation thread-local across coroutine thread switches. */
private class InvocationContextElement :
    AbstractCoroutineContextElement(InvocationContextKey), ThreadContextElement<InvocationState> {
    private val captured = current()

    private fun current(): InvocationState =
        InvocationState(ZLinkSuspendInvocationContext.snapshot(), ZLinkFlowContext.current())

    override fun updateThreadContext(context: CoroutineContext): InvocationState {
        val previous = current()
        captured.restore()
        return previous
    }

    override fun restoreThreadContext(context: CoroutineContext, oldState: InvocationState) {
        oldState.restore()
    }
}
