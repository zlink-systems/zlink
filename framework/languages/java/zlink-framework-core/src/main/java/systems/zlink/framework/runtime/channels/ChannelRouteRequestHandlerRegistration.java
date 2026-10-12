package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record ChannelRouteRequestHandlerRegistration(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName,
        PreparedMethod invocation) {
    ChannelRouteRequestHandlerRegistration(
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
                        handlerType, handlerMethod, requestType, ZLinkRouteMessageContext.class));
    }

    ChannelRouteRequestHandlerRegistration(
            Class<?> handlerType, Class<?> requestType, Class<?> replyType, String packetName) {
        this(handlerType, (Method) null, requestType, replyType, packetName);
    }

    Method handlerMethod() {
        return invocation.method();
    }

    ChannelRouteRequestHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelRouteRequestHandlerRegistration(
                handlerType,
                requestType,
                replyType,
                packetName,
                invocation.withSuspendInvokers(invokers));
    }

    ChannelRouteRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteRequestHandlerRegistration(
                handlerType, requestType, replyType, packetName, invocation);
    }
}
