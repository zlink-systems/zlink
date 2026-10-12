package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;
import java.util.List;

record ChannelPublishHandlerRegistration(
        Class<?> handlerType, Class<?> messageType, String packetName, PreparedMethod invocation) {
    ChannelPublishHandlerRegistration(
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
                        handlerType,
                        method,
                        messageType,
                        ZLinkPublishMessageContext.class,
                        invokers));
    }

    ChannelPublishHandlerRegistration(
            Class<?> handlerType,
            Class<?> messageType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(handlerType, null, messageType, packetName, invokers);
    }

    ChannelPublishHandlerRegistration(
            Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, null, messageType, packetName, List.of());
    }

    ChannelPublishHandlerRegistration withPacketName(String packetName) {
        return new ChannelPublishHandlerRegistration(
                handlerType, messageType, packetName, invocation);
    }
}
