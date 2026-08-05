package systems.zlink.framework.runtime.spots;

import java.lang.reflect.Method;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;

record SpotActorPacketHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> spotType,
    Class<?> actorType,
    Class<?> messageType,
    Class<?> replyType,
    String packetName,
    ZLinkScannedHandlerKind kind) {
}
