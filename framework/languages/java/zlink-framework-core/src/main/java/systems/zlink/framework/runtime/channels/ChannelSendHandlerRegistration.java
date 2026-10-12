package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelSendHandlerRegistration(
        Class<?> handlerType, Class<?> messageType, String packetName, PreparedMethod invocation) {
    ChannelSendHandlerRegistration(
            Class<?> handlerType, Method handlerMethod, Class<?> messageType, String packetName) {
        this(
                handlerType,
                messageType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType, handlerMethod, messageType, ZLinkMessageContext.class));
    }

    ChannelSendHandlerRegistration(Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, (Method) null, messageType, packetName);
    }

    Method handlerMethod() {
        return invocation.method();
    }

    ChannelSendHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelSendHandlerRegistration(
                handlerType, messageType, packetName, invocation.withSuspendInvokers(invokers));
    }

    ChannelSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelSendHandlerRegistration(handlerType, messageType, packetName, invocation);
    }
}
