package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import java.util.concurrent.LinkedBlockingQueue
import org.springframework.stereotype.Component

/**
 * Bounded hand-off queue from workflow handlers to the saga worker. A poison
 * sentinel unblocks [take] during shutdown without a busy loop.
 */
@Component
class WorkflowContinuationQueue {
    private val queue = LinkedBlockingQueue<String>()

    fun enqueue(orderId: String) {
        queue.add(orderId)
    }

    fun take(): String? {
        val orderId = queue.take()
        return if (orderId == POISON) null else orderId
    }

    fun signalStop() {
        queue.add(POISON)
    }

    private companion object {
        const val POISON = " "
    }
}
