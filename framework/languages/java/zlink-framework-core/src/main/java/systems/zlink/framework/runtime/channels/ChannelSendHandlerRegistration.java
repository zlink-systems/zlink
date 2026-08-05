package systems.zlink.framework.runtime.channels;

import java.lang.reflect.Method;
record ChannelSendHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> messageType,
    String packetName) {
    ChannelSendHandlerRegistration(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName) {
        this(handlerType, null, messageType, packetName);
    }

    ChannelSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelSendHandlerRegistration(
            handlerType,
            handlerMethod,
            messageType,
            packetName);
    }
}
