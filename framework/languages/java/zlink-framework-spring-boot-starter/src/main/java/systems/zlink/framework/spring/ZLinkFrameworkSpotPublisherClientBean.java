package systems.zlink.framework.spring;

import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

final class ZLinkFrameworkSpotPublisherClientBean implements ZLinkSpotPublisherClient {
    private final ZLinkFrameworkLifecycle lifecycle;

    ZLinkFrameworkSpotPublisherClientBean(ZLinkFrameworkLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    @Override
    public ZLinkPublishCall publish(
        String meshName,
        String channelName,
        String topic,
        Object message) {
        return lifecycle.spotPublisherClient().publish(
            meshName, channelName, topic, message);
    }

    @Override
    public ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        return lifecycle.spotPublisherClient().publish(channelName, topic, message);
    }
}
