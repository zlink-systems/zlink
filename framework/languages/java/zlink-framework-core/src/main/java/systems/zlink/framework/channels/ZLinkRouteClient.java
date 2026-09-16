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

    /** Sends a message to {@code target} in the named RouteMesh. */
    ZLinkSendCall sendToNode(
        String meshName,
        RoutingId target,
        Object message);

    ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message);

    /** Requests {@code target} in the named RouteMesh. */
    ZLinkRequestCall requestToNode(
        String meshName,
        RoutingId target,
        Object message);

    ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object message);
}
