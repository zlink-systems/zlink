package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;
import java.util.List;

record ChannelRouteRequestHandlerRegistration(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName,
        PreparedMethod invocation) {
    ChannelRouteRequestHandlerRegistration(
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
                        handlerType,
                        method,
                        requestType,
                        ZLinkRouteMessageContext.class,
                        invokers));
    }

    ChannelRouteRequestHandlerRegistration(
            Class<?> handlerType,
            Class<?> requestType,
            Class<?> replyType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(handlerType, null, requestType, replyType, packetName, invokers);
    }

    ChannelRouteRequestHandlerRegistration(
            Class<?> handlerType, Class<?> requestType, Class<?> replyType, String packetName) {
        this(handlerType, null, requestType, replyType, packetName, List.of());
    }

    ChannelRouteRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteRequestHandlerRegistration(
                handlerType, requestType, replyType, packetName, invocation);
    }
}
