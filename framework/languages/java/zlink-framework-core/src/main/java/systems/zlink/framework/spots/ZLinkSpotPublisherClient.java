package systems.zlink.framework.spots;

import systems.zlink.framework.channels.ZLinkPublishCall;

public interface ZLinkSpotPublisherClient {
    ZLinkPublishCall publish(
        String meshName,
        String channelName,
        String topic,
        Object message);

    ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message);
}
