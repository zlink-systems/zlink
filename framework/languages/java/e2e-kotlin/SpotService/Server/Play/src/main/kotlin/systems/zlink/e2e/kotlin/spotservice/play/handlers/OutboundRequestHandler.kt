package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import java.time.Duration
import systems.zlink.framework.handlers.ZLinkSpotRequest
import systems.zlink.framework.kotlin.await

class OutboundRequestHandler {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: UserSpot,
        request: Contracts.OutboundReq,
    ): Contracts.OutboundRes {
        val channelReply = spot.context()
            .outbound()
            .requestToChannel(Contracts.INGRESS_CHANNEL, request.value)
            .timeout(Duration.ofSeconds(5))
            .submit(String::class.java).await()
        spot.context()
            .outbound()
            .sendToChannel(
                Contracts.INGRESS_CHANNEL,
                Contracts.OutboundMsg("send:${request.value}"),
            )
            .submit()
        spot.context()
            .outbound()
            .publish(
                Contracts.ROUTE_CHANNEL,
                "spot.events",
                Contracts.MeshMsg("publish:${request.value}"),
            )
            .submit()
        spot.record("SpotOutbound", "${request.value}/$channelReply")
        return Contracts.OutboundRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            channelReply,
        )
    }
}
