package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import jakarta.annotation.PostConstruct
import jakarta.annotation.PreDestroy
import java.util.concurrent.atomic.AtomicBoolean
import org.springframework.stereotype.Component
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RunOrderWorkflowMsg

/**
 * Drives the asynchronous part of the checkout saga. Workflow handlers reply to
 * `StartOrderWorkflowReq` as soon as the `Created` projection exists; this worker then advances
 * inventory, payment, and confirmation in the background.
 */
@Component
class WorkflowSagaWorker(
    private val queue: WorkflowContinuationQueue,
    private val routes: ZLinkRouteClient,
) {
    private val running = AtomicBoolean(false)
    private var worker: Thread? = null

    @PostConstruct
    fun start() {
        running.set(true)
        worker =
            Thread({ pump() }, "shoppingmall-workflow-saga-worker").apply {
                isDaemon = true
                start()
            }
    }

    @PreDestroy
    fun stop() {
        running.set(false)
        queue.signalStop()
        worker?.interrupt()
    }

    // --8<-- [start:doc-sm-background-continue]
    private fun pump() {
        while (running.get()) {
            val orderId =
                try {
                    queue.take()
                } catch (ignored: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return
                } ?: return
            try {
                routes
                    .sendToSpot(orderId, RunOrderWorkflowMsg(orderId))
                    .instanceSpot(SampleNames.OrderWorkflowSpotType)
                    .inMesh(SampleNames.OrderWorkflowMesh)
                    .submit()
                    .toCompletableFuture()
                    .join()
                System.err.println("shoppingmall order: advanced order=$orderId")
            } catch (error: RuntimeException) {
                System.err.println(
                    "shoppingmall order: saga failed order=$orderId error=${error.message}"
                )
            }
        }
    }
    // --8<-- [end:doc-sm-background-continue]
}
