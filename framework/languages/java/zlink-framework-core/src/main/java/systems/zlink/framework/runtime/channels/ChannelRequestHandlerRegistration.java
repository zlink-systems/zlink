package systems.zlink.framework.runtime.channels;

import java.lang.reflect.Method;
record ChannelRequestHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> requestType,
    Class<?> replyType,
    String packetName) {
    ChannelRequestHandlerRegistration(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName) {
        this(handlerType, null, requestType, replyType, packetName);
    }

    ChannelRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRequestHandlerRegistration(
            handlerType,
            handlerMethod,
            requestType,
            replyType,
            packetName);
    }
}
