package systems.zlink.framework.runtime.channels;

import java.lang.reflect.Method;
record ChannelRouteRequestHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> requestType,
    Class<?> replyType,
    String packetName) {
    ChannelRouteRequestHandlerRegistration(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName) {
        this(handlerType, null, requestType, replyType, packetName);
    }

    ChannelRouteRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteRequestHandlerRegistration(
            handlerType,
            handlerMethod,
            requestType,
            replyType,
            packetName);
    }
}
