package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelPublishHandlerRegistration(
        Class<?> handlerType, Class<?> messageType, String packetName, PreparedMethod invocation) {
    ChannelPublishHandlerRegistration(
            Class<?> handlerType, Method handlerMethod, Class<?> messageType, String packetName) {
        this(
                handlerType,
                messageType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType, handlerMethod, messageType, ZLinkPublishMessageContext.class));
    }

    ChannelPublishHandlerRegistration(
            Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, (Method) null, messageType, packetName);
    }

    Method handlerMethod() {
        return invocation.method();
    }

    ChannelPublishHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelPublishHandlerRegistration(
                handlerType, messageType, packetName, invocation.withSuspendInvokers(invokers));
    }

    ChannelPublishHandlerRegistration withPacketName(String packetName) {
        return new ChannelPublishHandlerRegistration(
                handlerType, messageType, packetName, invocation);
    }
}
