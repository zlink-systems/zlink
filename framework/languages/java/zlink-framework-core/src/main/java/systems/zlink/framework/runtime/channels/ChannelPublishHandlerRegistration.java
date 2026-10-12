package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelPublishHandlerRegistration(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName,
        Method declaredMethod,
        PreparedMethod invocation) {
    ChannelPublishHandlerRegistration(
            Class<?> handlerType, Method declaredMethod, Class<?> messageType, String packetName) {
        this(handlerType, messageType, packetName, declaredMethod, null);
    }

    ChannelPublishHandlerRegistration(
            Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, (Method) null, messageType, packetName);
    }

    ChannelPublishHandlerRegistration prepare(Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelPublishHandlerRegistration(
                handlerType,
                messageType,
                packetName,
                null,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType,
                        declaredMethod,
                        messageType,
                        ZLinkPublishMessageContext.class,
                        invokers));
    }

    ChannelPublishHandlerRegistration withPacketName(String packetName) {
        return new ChannelPublishHandlerRegistration(
                handlerType, messageType, packetName, declaredMethod, invocation);
    }
}
