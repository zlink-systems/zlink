package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;
import java.util.List;

record ChannelRouteSendHandlerRegistration(
        Class<?> handlerType, Class<?> messageType, String packetName, PreparedMethod invocation) {
    ChannelRouteSendHandlerRegistration(
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
                        ZLinkRouteMessageContext.class,
                        invokers));
    }

    ChannelRouteSendHandlerRegistration(
            Class<?> handlerType,
            Class<?> messageType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(handlerType, null, messageType, packetName, invokers);
    }

    ChannelRouteSendHandlerRegistration(
            Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, null, messageType, packetName, List.of());
    }

    ChannelRouteSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteSendHandlerRegistration(
                handlerType, messageType, packetName, invocation);
    }
}
