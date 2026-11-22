package systems.zlink.framework.kotlin

import kotlin.coroutines.AbstractCoroutineContextElement
import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.ThreadContextElement
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext

internal object ZLinkCoroutineInvocationContext {
    fun capture(dispatcher: CoroutineDispatcher): CoroutineContext =
        dispatcher + Invocation(snapshot())

    private data class Snapshot(
        val invocation: ZLinkSuspendInvocationContext.Snapshot,
        val flow: ZLinkFlowContext.State?,
    ) {
        fun restore() {
            ZLinkSuspendInvocationContext.restore(invocation)
            val local = ZLinkFlowContext.threadLocal()
            if (flow == null) local.remove() else local.set(flow)
        }
    }

    private fun snapshot(): Snapshot =
        Snapshot(ZLinkSuspendInvocationContext.snapshot(), ZLinkFlowContext.current())

    private class Invocation(private val captured: Snapshot) :
        AbstractCoroutineContextElement(Key), ThreadContextElement<Snapshot> {
        companion object Key : CoroutineContext.Key<Invocation>

        override fun updateThreadContext(context: CoroutineContext): Snapshot {
            val previous = snapshot()
            captured.restore()
            return previous
        }

        override fun restoreThreadContext(context: CoroutineContext, oldState: Snapshot) {
            oldState.restore()
        }
    }
}
