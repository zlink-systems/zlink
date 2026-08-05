package systems.zlink.framework.channels;

public interface ZLinkClient {
    ZLinkSendCall sendToChannel(
        String channelName,
        Object message);

    ZLinkRequestCall requestToChannel(
        String channelName,
        Object message);
}
