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
        PreparedMethod invocation) {
    ChannelRequestHandlerRegistration(
            Class<?> handlerType,
            Method handlerMethod,
            Class<?> requestType,
            Class<?> replyType,
            String packetName) {
        this(
                handlerType,
                requestType,
                replyType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType, handlerMethod, requestType, ZLinkMessageContext.class));
    }

    ChannelRequestHandlerRegistration(
            Class<?> handlerType, Class<?> requestType, Class<?> replyType, String packetName) {
        this(handlerType, (Method) null, requestType, replyType, packetName);
    }

    Method handlerMethod() {
        return invocation.method();
    }

    ChannelRequestHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelRequestHandlerRegistration(
                handlerType,
                requestType,
                replyType,
                packetName,
                invocation.withSuspendInvokers(invokers));
    }

    ChannelRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRequestHandlerRegistration(
                handlerType, requestType, replyType, packetName, invocation);
    }
}
