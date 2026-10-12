package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker.PreparedMethod;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;

import java.lang.reflect.Method;
import java.util.Collection;

record SpotActorPacketHandlerRegistration(
        Class<?> handlerType,
        Class<?> spotType,
        Class<?> actorType,
        Class<?> messageType,
        Class<?> replyType,
        String packetName,
        ZLinkScannedHandlerKind kind,
        PreparedMethod invocation) {
    SpotActorPacketHandlerRegistration(
            Class<?> handlerType,
            Method method,
            Class<?> spotType,
            Class<?> actorType,
            Class<?> messageType,
            Class<?> replyType,
            String packetName,
            ZLinkScannedHandlerKind kind) {
        this(
                handlerType,
                spotType,
                actorType,
                messageType,
                replyType,
                packetName,
                kind,
                ZLinkHandlerMethodInvoker.prepareActor(
                        handlerType,
                        method,
                        spotType,
                        actorType,
                        messageType,
                        ZLinkMessageContext.class));
    }

    Method handlerMethod() {
        return invocation.method();
    }

    SpotActorPacketHandlerRegistration withSuspendInvokers(
            Collection<ZLinkSuspendInvocationAdapter> invokers) {
        return new SpotActorPacketHandlerRegistration(
                handlerType,
                spotType,
                actorType,
                messageType,
                replyType,
                packetName,
                kind,
                invocation.withSuspendInvokers(invokers));
    }
}
