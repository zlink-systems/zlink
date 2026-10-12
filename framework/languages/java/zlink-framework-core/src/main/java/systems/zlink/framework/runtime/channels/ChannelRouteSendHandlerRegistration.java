package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelRouteSendHandlerRegistration(
        Class<?> handlerType, Class<?> messageType, String packetName, PreparedMethod invocation) {
    ChannelRouteSendHandlerRegistration(
            Class<?> handlerType, Method handlerMethod, Class<?> messageType, String packetName) {
        this(
                handlerType,
                messageType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType, handlerMethod, messageType, ZLinkRouteMessageContext.class));
    }

    ChannelRouteSendHandlerRegistration(
            Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, (Method) null, messageType, packetName);
    }

    Method handlerMethod() {
        return invocation.method();
    }

    ChannelRouteSendHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelRouteSendHandlerRegistration(
                handlerType, messageType, packetName, invocation.withSuspendInvokers(invokers));
    }

    ChannelRouteSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteSendHandlerRegistration(
                handlerType, messageType, packetName, invocation);
    }
}
