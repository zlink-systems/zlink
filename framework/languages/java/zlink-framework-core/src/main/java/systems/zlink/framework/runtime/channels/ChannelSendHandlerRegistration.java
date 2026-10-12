package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;
import java.util.List;

record ChannelSendHandlerRegistration(
        Class<?> handlerType, Class<?> messageType, String packetName, PreparedMethod invocation) {
    ChannelSendHandlerRegistration(
            Class<?> handlerType,
            Method method,
            Class<?> messageType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(
                handlerType,
                messageType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType, method, messageType, ZLinkMessageContext.class, invokers));
    }

    ChannelSendHandlerRegistration(
            Class<?> handlerType,
            Class<?> messageType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(handlerType, null, messageType, packetName, invokers);
    }

    ChannelSendHandlerRegistration(Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, null, messageType, packetName, List.of());
    }

    ChannelSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelSendHandlerRegistration(handlerType, messageType, packetName, invocation);
    }
}
