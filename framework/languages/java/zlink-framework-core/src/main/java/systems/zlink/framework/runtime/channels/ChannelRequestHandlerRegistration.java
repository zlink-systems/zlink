package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;
import java.util.List;

record ChannelRequestHandlerRegistration(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName,
        PreparedMethod invocation) {
    ChannelRequestHandlerRegistration(
            Class<?> handlerType,
            Method method,
            Class<?> requestType,
            Class<?> replyType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(
                handlerType,
                requestType,
                replyType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType, method, requestType, ZLinkMessageContext.class, invokers));
    }

    ChannelRequestHandlerRegistration(
            Class<?> handlerType,
            Class<?> requestType,
            Class<?> replyType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(handlerType, null, requestType, replyType, packetName, invokers);
    }

    ChannelRequestHandlerRegistration(
            Class<?> handlerType, Class<?> requestType, Class<?> replyType, String packetName) {
        this(handlerType, null, requestType, replyType, packetName, List.of());
    }

    ChannelRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRequestHandlerRegistration(
                handlerType, requestType, replyType, packetName, invocation);
    }
}
