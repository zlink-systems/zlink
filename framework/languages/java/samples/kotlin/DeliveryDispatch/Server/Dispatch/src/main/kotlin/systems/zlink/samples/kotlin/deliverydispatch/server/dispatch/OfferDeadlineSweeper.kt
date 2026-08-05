package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTimings

/**
 * The offer deadline. It is a timer, not a wait: nothing is blocked on a courier, so a lapsed offer
 * only moves on because someone comes looking for it (common sample spec section 7.4).
 */
class OfferDeadlineSweeper(
    private val offers: DeliveryOfferStore,
    private val worker: DispatchWorker,
) : AutoCloseable {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    init {
        scope.launch {
            while (isActive) {
                delay(SampleTimings.OfferSweepInterval.toMillis())
                sweep()
            }
        }
    }

    override fun close() {
        scope.cancel()
    }

    private suspend fun sweep() {
        for (offer in offers.takeExpired()) {
            println(
                "deliverydispatch dispatch: offer expired delivery=${offer.request.deliveryId} " +
                    "attempt=${offer.attempt}",
            )
            try {
                worker.reassign(offer)
            } catch (error: RuntimeException) {
                // The sweeper comes back next tick; one failed reassign must not stop it.
                System.err.println(
                    "deliverydispatch dispatch: reassign failed " +
                        "delivery=${offer.request.deliveryId}: ${error.message}",
                )
            }
        }
    }
}
