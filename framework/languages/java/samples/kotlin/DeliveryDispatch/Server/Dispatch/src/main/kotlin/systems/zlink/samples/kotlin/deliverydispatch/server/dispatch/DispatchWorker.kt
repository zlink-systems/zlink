package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import java.time.Instant
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.kotlin.await
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.AssignDeliveryMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatus
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusChangedReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusChangedRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionRes

/**
 * The dispatch flow. No step of it waits for a courier: the offer goes out one-way, the turn ends,
 * and the offer row decides what happens next — either a decision arrives, or the deadline passes
 * and the sweeper reassigns (common sample spec section 7.4).
 */
class DispatchWorker(
    private val channels: ZLinkClient,
    private val actors: ZLinkActorClient,
    private val offers: DeliveryOfferStore,
) {
    /** Who gets offered a delivery, and in what order. The worker's policy, not the node's. */
    private val candidates = listOf("courier-a", "courier-b")

    /** The first offer. Records it, sends it, and returns — nobody is left waiting. */
    suspend fun dispatch(request: AssignDeliveryMsg) {
        val courierId = candidates[0]
        val attempt = offers.offer(request, 0, SampleTimings.CourierDecisionTimeout)
        publishStatus(request, DeliveryStatus.Assigned, courierId)
        offer(request, courierId, attempt)
    }

    /** A decision arrived. Accepted carries the delivery through; refused reassigns. */
    suspend fun settle(offer: DeliveryOffer, accepted: Boolean, reason: String?) {
        val courierId = candidates[offer.candidateIndex]
        if (!accepted) {
            println(
                "deliverydispatch dispatch: courier=$courierId did not take " +
                    "delivery=${offer.request.deliveryId} (${reason ?: "refused"})",
            )
            reassign(offer)
            return
        }

        publishStatus(offer.request, DeliveryStatus.Accepted, courierId)
        publishStatus(offer.request, DeliveryStatus.PickedUp, courierId)
        publishStatus(offer.request, DeliveryStatus.Delivered, courierId)
        offers.close(offer.request.deliveryId)
    }

    /**
     * The offer lapsed, or the courier refused. Either way the next candidate gets it. The deadline
     * lives here rather than on the courier node: a node that timed the offer and manufactured a
     * refusal would be hiding the dispatch policy (common sample spec section 7.4).
     */
    suspend fun reassign(offer: DeliveryOffer) {
        val nextIndex = offer.candidateIndex + 1
        if (nextIndex >= candidates.size) {
            System.err.println(
                "deliverydispatch dispatch: delivery=${offer.request.deliveryId} " +
                    "was rejected by all couriers",
            )
            publishStatus(offer.request, DeliveryStatus.Failed, candidates.last())
            offers.close(offer.request.deliveryId)
            return
        }

        val courierId = candidates[nextIndex]
        val attempt = offers.offer(offer.request, nextIndex, SampleTimings.CourierDecisionTimeout)
        publishStatus(offer.request, DeliveryStatus.Reassigned, courierId)
        offer(offer.request, courierId, attempt)
    }

    suspend fun assertServerEvidence(request: ServerAssertionReq): ServerAssertionRes =
        channels
            .requestToChannel(SampleNames.TrackingChannel, request)
            .submit(ServerAssertionRes::class.java).await()

    /** The offer is a one-way send: the turn that sends it ends right there. */
    private suspend fun offer(request: AssignDeliveryMsg, courierId: String, attempt: Int) {
        actors
            .sendToActor(
                courierId,
                OfferDeliveryMsg(
                    courierId = courierId,
                    deliveryId = request.deliveryId,
                    attempt = attempt,
                    pickupAddress = request.pickupAddress,
                    dropoffAddress = request.dropoffAddress,
                ),
            )
            .submit()
            .await()
    }

    private suspend fun publishStatus(
        delivery: AssignDeliveryMsg,
        status: DeliveryStatus,
        courierId: String,
    ) {
        channels
            .requestToChannel(
                SampleNames.TrackingChannel,
                DeliveryStatusChangedReq(
                    deliveryId = delivery.deliveryId,
                    customerId = delivery.customerId,
                    status = status,
                    courierId = courierId,
                    occurredAt = Instant.now(),
                ),
            )
            .submit(DeliveryStatusChangedRes::class.java).await()
    }
}
