package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelSendHandlerRegistration(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName,
        Method declaredMethod,
        PreparedMethod invocation) {
    ChannelSendHandlerRegistration(
            Class<?> handlerType, Method declaredMethod, Class<?> messageType, String packetName) {
        this(handlerType, messageType, packetName, declaredMethod, null);
    }

    ChannelSendHandlerRegistration(Class<?> handlerType, Class<?> messageType, String packetName) {
        this(handlerType, (Method) null, messageType, packetName);
    }

    ChannelSendHandlerRegistration prepare(Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelSendHandlerRegistration(
                handlerType,
                messageType,
                packetName,
                null,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType,
                        declaredMethod,
                        messageType,
                        ZLinkMessageContext.class,
                        invokers));
    }

    ChannelSendHandlerRegistration withPacketName(String packetName) {
        return new ChannelSendHandlerRegistration(
                handlerType, messageType, packetName, declaredMethod, invocation);
    }
}
