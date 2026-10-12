package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelRouteSendHandlerRegistration(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName,
        Method declaredMethod,
        PreparedMethod invocation) {
    ChannelRouteSendHandlerRegistration(
            Class<?> handlerType, Method declaredMethod, Class<?> messageType, String packetName) {
        this(handlerType, messageType, packetName, declaredMethod, null);
    }

    ChannelRouteSendHandlerRegistration(
            Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, (Method) null, messageType, packetName);
    }

    ChannelRouteSendHandlerRegistration prepare(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelRouteSendHandlerRegistration(
                handlerType,
                messageType,
                packetName,
                null,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType,
                        declaredMethod,
                        messageType,
                        ZLinkRouteMessageContext.class,
                        invokers));
    }

    ChannelRouteSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteSendHandlerRegistration(
                handlerType, messageType, packetName, declaredMethod, invocation);
    }
}
