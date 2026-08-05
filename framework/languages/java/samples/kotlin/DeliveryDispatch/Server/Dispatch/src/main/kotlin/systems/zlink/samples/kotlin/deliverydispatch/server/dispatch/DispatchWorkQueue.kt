package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlinx.coroutines.runBlocking
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.AssignDeliveryMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CreateDeliveryReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionRes

class DispatchWorkQueue(
    private val worker: DispatchWorker,
) : AutoCloseable {
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()

    fun enqueue(request: CreateDeliveryReq) {
        executor.submit {
            try {
                println("deliverydispatch-dispatch-start=${request.deliveryId}")
                runBlocking { worker.dispatch(
                    AssignDeliveryMsg(
                        deliveryId = request.deliveryId,
                        customerId = request.customerId,
                        pickupAddress = request.pickupAddress,
                        dropoffAddress = request.dropoffAddress,
                    ),
                ) }
                println("deliverydispatch-dispatch-finished=${request.deliveryId}")
            } catch (ex: RuntimeException) {
                System.err.println("deliverydispatch-dispatch-failed=${request.deliveryId}: ${ex.message}")
                ex.printStackTrace(System.err)
            }
        }
    }

    fun assertServerEvidence(request: ServerAssertionReq): ServerAssertionRes =
        runBlocking { worker.assertServerEvidence(request) }

    override fun close() {
        executor.shutdownNow()
    }
}
