package systems.zlink.e2e.spotservice.shared;

import java.time.Duration;
import systems.zlink.framework.handlers.ZLinkSpotRequest;

public final class OutboundReqHandler {
    @ZLinkSpotRequest
    public java.util.concurrent.CompletionStage<Contracts.OutboundRes> handle(
        UserSpot spot,
        Contracts.OutboundReq request) {
        return spot.context()
            .outbound()
            .requestToChannel(Contracts.INGRESS_CHANNEL, new Contracts.StateReq(request.value()))
            .timeout(Duration.ofSeconds(5))
            .submit(String.class)
            .thenApply(channelReply -> {
                spot.context().outbound().sendToChannel(Contracts.INGRESS_CHANNEL,
                    new Contracts.OutboundMsg("send:" + request.value())).submit();
                spot.context().outbound().publish(Contracts.ROUTE_CHANNEL, "spot.events",
                    new Contracts.MeshMsg("publish:" + request.value())).submit();
                spot.record("SpotOutbound", request.value() + "/" + channelReply);
                return new Contracts.OutboundRes(spot.context().spotId(),
                    spot.context().nodeRid().toString(), channelReply);
            });
    }
}
