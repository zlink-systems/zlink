package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record SpotPacketHandlerRegistration(
        Class<?> handlerType,
        Class<?> spotType,
        Class<?> messageType,
        Class<?> replyType,
        String packetName,
        boolean request,
        PreparedMethod invocation) {
    SpotPacketHandlerRegistration(
            Class<?> handlerType,
            Method method,
            Class<?> spotType,
            Class<?> messageType,
            Class<?> replyType,
            String packetName,
            boolean request,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(
                handlerType,
                spotType,
                messageType,
                replyType,
                packetName,
                request,
                ZLinkHandlerMethodInvoker.prepareSpot(
                        handlerType,
                        method,
                        spotType,
                        messageType,
                        ZLinkMessageContext.class,
                        invokers));
    }
}

record SpotSubscriptionHandlerRegistration(
        String topic,
        Class<?> handlerType,
        Class<?> spotType,
        Class<?> messageType,
        String packetName,
        PreparedMethod invocation) {
    SpotSubscriptionHandlerRegistration(
            String topic,
            Class<?> handlerType,
            Method method,
            Class<?> spotType,
            Class<?> messageType,
            String packetName,
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        this(
                topic,
                handlerType,
                spotType,
                messageType,
                packetName,
                ZLinkHandlerMethodInvoker.prepareSpot(
                        handlerType,
                        method,
                        spotType,
                        messageType,
                        ZLinkPublishMessageContext.class,
                        invokers));
    }
}
