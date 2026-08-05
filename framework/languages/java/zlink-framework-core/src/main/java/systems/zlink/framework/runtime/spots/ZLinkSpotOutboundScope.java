package systems.zlink.framework.runtime.spots;

import java.util.function.Supplier;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.ZLinkSpotOutbound;

final class ZLinkSpotOutboundScope {
    private final ZLinkSpotOutbound ambient = new ZLinkAmbientSpotOutbound(this);

    ZLinkSpotOutbound ambient() {
        return ambient;
    }

    <T> T run(DefaultSpotOutbound outbound, Supplier<T> action) {
        try (systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterSpotOutbound(outbound)) {
            return action.get();
        }
    }

    DefaultSpotOutbound requireCurrent() {
        DefaultSpotOutbound outbound = (DefaultSpotOutbound) systems.zlink.framework.runtime
            .internal.handlers.ZLinkSuspendInvocationContext.currentSpotOutbound();
        if (outbound == null) {
            throw new ZLinkConfigurationException(
                "ZLinkSpotOutbound can only be used inside an active Spot callback");
        }
        return outbound;
    }

}

final class ZLinkAmbientSpotOutbound implements ZLinkSpotOutbound {
    private final ZLinkSpotOutboundScope scope;

    ZLinkAmbientSpotOutbound(ZLinkSpotOutboundScope scope) {
        this.scope = scope;
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message) {
        return scope.requireCurrent().sendToSpot(spotId, message);
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object request) {
        return scope.requireCurrent().requestToSpot(spotId, request);
    }

    @Override
    public ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        return scope.requireCurrent().publish(channelName, topic, message);
    }

    @Override
    public ZLinkSendCall sendToChannel(String channelName, Object message) {
        return scope.requireCurrent().sendToChannel(channelName, message);
    }

    @Override
    public ZLinkRequestCall requestToChannel(String channelName, Object request) {
        return scope.requireCurrent().requestToChannel(channelName, request);
    }
}
