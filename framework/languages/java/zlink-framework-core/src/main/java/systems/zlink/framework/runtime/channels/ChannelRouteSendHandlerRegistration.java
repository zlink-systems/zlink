package systems.zlink.framework.runtime.channels;

import java.lang.reflect.Method;
record ChannelRouteSendHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> messageType,
    String packetName) {
    ChannelRouteSendHandlerRegistration(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName) {
        this(handlerType, null, messageType, packetName);
    }

    ChannelRouteSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteSendHandlerRegistration(
            handlerType,
            handlerMethod,
            messageType,
            packetName);
    }
}
