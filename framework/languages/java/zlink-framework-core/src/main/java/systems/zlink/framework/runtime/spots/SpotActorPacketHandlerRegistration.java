package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;

import java.lang.reflect.Method;

record SpotActorPacketHandlerRegistration(
        Class<?> handlerType,
        Method handlerMethod,
        Class<?> spotType,
        Class<?> actorType,
        Class<?> messageType,
        Class<?> replyType,
        String packetName,
        ZLinkScannedHandlerKind kind) {}
