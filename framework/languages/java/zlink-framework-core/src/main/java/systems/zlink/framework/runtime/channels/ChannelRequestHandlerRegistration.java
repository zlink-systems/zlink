package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelRequestHandlerRegistration(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName,
        Method declaredMethod,
        PreparedMethod invocation) {
    ChannelRequestHandlerRegistration(
            Class<?> handlerType,
            Method declaredMethod,
            Class<?> requestType,
            Class<?> replyType,
            String packetName) {
        this(handlerType, requestType, replyType, packetName, declaredMethod, null);
    }

    ChannelRequestHandlerRegistration(
            Class<?> handlerType, Class<?> requestType, Class<?> replyType, String packetName) {
        this(handlerType, (Method) null, requestType, replyType, packetName);
    }

    ChannelRequestHandlerRegistration prepare(Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelRequestHandlerRegistration(
                handlerType,
                requestType,
                replyType,
                packetName,
                null,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType,
                        declaredMethod,
                        requestType,
                        ZLinkMessageContext.class,
                        invokers));
    }

    ChannelRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRequestHandlerRegistration(
                handlerType, requestType, replyType, packetName, declaredMethod, invocation);
    }
}
