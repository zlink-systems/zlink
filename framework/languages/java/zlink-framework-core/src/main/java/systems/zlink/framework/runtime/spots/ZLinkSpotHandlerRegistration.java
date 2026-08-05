package systems.zlink.framework.runtime.spots;

import java.lang.reflect.Method;

record SpotPacketHandlerRegistration(
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> spotType,
    Class<?> messageType,
    Class<?> replyType,
    String packetName,
    boolean request) {
}

record SpotSubscriptionHandlerRegistration(
    String topic,
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> spotType,
    Class<?> messageType,
    String packetName) {
}
