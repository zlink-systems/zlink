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
        Method declaredMethod,
        PreparedMethod invocation) {
    ChannelRouteRequestHandlerRegistration(
            Class<?> handlerType,
            Method declaredMethod,
            Class<?> requestType,
            Class<?> replyType,
            String packetName) {
        this(handlerType, requestType, replyType, packetName, declaredMethod, null);
    }

    ChannelRouteRequestHandlerRegistration(
            Class<?> handlerType, Class<?> requestType, Class<?> replyType, String packetName) {
        this(handlerType, (Method) null, requestType, replyType, packetName);
    }

    ChannelRouteRequestHandlerRegistration prepare(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new ChannelRouteRequestHandlerRegistration(
                handlerType,
                requestType,
                replyType,
                packetName,
                null,
                ZLinkHandlerMethodInvoker.prepareChannel(
                        handlerType,
                        declaredMethod,
                        requestType,
                        ZLinkRouteMessageContext.class,
                        invokers));
    }

    ChannelRouteRequestHandlerRegistration withPacketName(String packetName) {
        return new ChannelRouteRequestHandlerRegistration(
                handlerType, requestType, replyType, packetName, declaredMethod, invocation);
    }
}
