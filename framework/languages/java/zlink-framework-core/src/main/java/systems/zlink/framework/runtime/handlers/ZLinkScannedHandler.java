package systems.zlink.framework.runtime.handlers;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.Set;

public record ZLinkScannedHandler(
    ZLinkScannedHandlerSurface surface,
    ZLinkScannedHandlerKind kind,
    Class<?> handlerType,
    Method handlerMethod,
    Class<?> spotType,
    Class<?> messageType,
    Class<?> replyType,
    String packetName,
    String topic,
    String timerName,
    Duration timerPeriod,
    Set<String> groups) {
    public ZLinkScannedHandler(
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind,
        Class<?> handlerType,
        Class<?> messageType,
        Class<?> replyType,
        String packetName,
        Set<String> groups) {
        this(surface, kind, handlerType, null, null, messageType, replyType, packetName,
            "", "", null, groups);
    }

    public ZLinkScannedHandler(
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind,
        Class<?> handlerType,
        Method handlerMethod,
        Class<?> messageType,
        Class<?> replyType,
        String packetName,
        Set<String> groups) {
        this(surface, kind, handlerType, handlerMethod, null, messageType, replyType,
            packetName, "", "", null, groups);
    }
}
