package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import java.time.Duration
import java.time.Instant
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.AssignDeliveryMsg

/** One delivery's offer in flight. The row decides what happens next, so nothing has to wait. */
data class DeliveryOffer(
    val request: AssignDeliveryMsg,
    val candidateIndex: Int,
    val attempt: Int,
    val deadline: Instant,
)

/**
 * The offers dispatch is waiting on. This is the whole of the waiting: no blocked thread and no
 * held serial queue anywhere, just rows with deadlines (common sample spec section 7.4).
 */
class DeliveryOfferStore {
    private class MutableOffer(
        var request: AssignDeliveryMsg,
        var candidateIndex: Int = 0,
        var attempt: Int = 0,
        var deadline: Instant = Instant.EPOCH,
        var settled: Boolean = false,
    )

    private val offers = mutableMapOf<String, MutableOffer>()

    /** Records a new offer and returns its attempt number. */
    fun offer(request: AssignDeliveryMsg, candidateIndex: Int, timeout: Duration): Int =
        synchronized(offers) {
            val offer = offers.getOrPut(request.deliveryId) { MutableOffer(request) }
            offer.request = request
            offer.candidateIndex = candidateIndex
            offer.attempt += 1
            offer.deadline = Instant.now().plus(timeout)
            offer.settled = false
            offer.attempt
        }

    /**
     * Closes the offer a decision belongs to and returns it. A decision naming another attempt
     * arrived after the offer was reassigned, and an offer already settled has been answered — both
     * are dropped.
     */
    fun settle(deliveryId: String, attempt: Int): DeliveryOffer? =
        synchronized(offers) {
            val offer = offers[deliveryId] ?: return null
            if (offer.settled || offer.attempt != attempt) return null
            offer.settled = true
            offer.snapshot()
        }

    /** The offers whose deadline has passed. The sweeper reassigns them. */
    fun takeExpired(): List<DeliveryOffer> {
        val now = Instant.now()
        return synchronized(offers) {
            offers.values
                .filter { !it.settled && !it.deadline.isAfter(now) }
                .onEach { it.settled = true }
                .map { it.snapshot() }
        }
    }

    fun close(deliveryId: String) {
        synchronized(offers) { offers.remove(deliveryId) }
    }

    private fun MutableOffer.snapshot(): DeliveryOffer =
        DeliveryOffer(request, candidateIndex, attempt, deadline)
}
