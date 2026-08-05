package systems.zlink.framework.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;
import systems.zlink.framework.spots.ZLinkSpotSendCall;

public interface ZLinkRouteClient {
    ZLinkSendCall sendToChannel(
        String channelName,
        Object message);

    ZLinkRequestCall requestToChannel(
        String channelName,
        Object request);

    ZLinkSendCall sendToNode(
        String channelName,
        RoutingId target,
        Object message);

    ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message);

    ZLinkRequestCall requestToNode(
        String channelName,
        RoutingId target,
        Object message);

    ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object message);
}
