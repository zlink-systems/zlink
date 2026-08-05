package systems.zlink.framework.runtime.channels;

import java.lang.reflect.Method;
record ChannelPublishHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> messageType,
    String packetName) {
    ChannelPublishHandlerRegistration(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName) {
        this(handlerType, null, messageType, packetName);
    }

    ChannelPublishHandlerRegistration withPacketName(String packetName) {
        return new ChannelPublishHandlerRegistration(
            handlerType,
            handlerMethod,
            messageType,
            packetName);
    }
}
