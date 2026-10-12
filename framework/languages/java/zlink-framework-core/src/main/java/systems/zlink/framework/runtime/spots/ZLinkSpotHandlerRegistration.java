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
            boolean request) {
        this(
                handlerType,
                spotType,
                messageType,
                replyType,
                packetName,
                request,
                ZLinkHandlerMethodInvoker.prepareSpot(
                        handlerType, method, spotType, messageType, ZLinkMessageContext.class));
    }

    Method handlerMethod() {
        return invocation.method();
    }

    SpotPacketHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new SpotPacketHandlerRegistration(
                handlerType,
                spotType,
                messageType,
                replyType,
                packetName,
                request,
                invocation.withSuspendInvokers(invokers));
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
            String packetName) {
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
                        ZLinkPublishMessageContext.class));
    }

    Method handlerMethod() {
        return invocation.method();
    }

    SpotSubscriptionHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new SpotSubscriptionHandlerRegistration(
                topic,
                handlerType,
                spotType,
                messageType,
                packetName,
                invocation.withSuspendInvokers(invokers));
    }
}
