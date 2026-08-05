package systems.zlink.framework.spring;

import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.ZLinkSpotOutbound;

final class ZLinkFrameworkSpotOutboundBean implements ZLinkSpotOutbound {
    private final ZLinkFrameworkLifecycle lifecycle;

    ZLinkFrameworkSpotOutboundBean(ZLinkFrameworkLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message) {
        return lifecycle.spotOutbound().sendToSpot(spotId, message);
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object request) {
        return lifecycle.spotOutbound().requestToSpot(spotId, request);
    }

    @Override
    public ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        return lifecycle.spotOutbound().publish(channelName, topic, message);
    }

    @Override
    public ZLinkSendCall sendToChannel(
        String channelName,
        Object message) {
        return lifecycle.spotOutbound().sendToChannel(channelName, message);
    }

    @Override
    public ZLinkRequestCall requestToChannel(
        String channelName,
        Object request) {
        return lifecycle.spotOutbound().requestToChannel(channelName, request);
    }
}
