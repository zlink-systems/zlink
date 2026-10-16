package systems.zlink.framework.spots;

public interface ZLinkSpotHandlerRegistry {
    void addHandler(Class<?> handlerType);

    void addSubscribe(Class<?> handlerType, String channelName, String topic);
}
