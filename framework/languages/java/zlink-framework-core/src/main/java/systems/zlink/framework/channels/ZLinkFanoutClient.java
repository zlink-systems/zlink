package systems.zlink.framework.channels;

public interface ZLinkFanoutClient {
    ZLinkFanoutPublishCall publish(
        String channelName,
        Object message);

    ZLinkFanoutPublishCall publish(
        String channelName,
        String topic,
        Object message);
}
