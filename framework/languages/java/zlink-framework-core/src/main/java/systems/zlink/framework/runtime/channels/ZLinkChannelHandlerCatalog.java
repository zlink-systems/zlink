package systems.zlink.framework.runtime.channels;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;

final class ZLinkChannelHandlerCatalog {
    private final ZLinkScannedHandlerCatalog scannedHandlers;

    ZLinkChannelHandlerCatalog(ZLinkScannedHandlerCatalog scannedHandlers) {
        this.scannedHandlers = scannedHandlers;
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    Map<String, ChannelRequestHandlerRegistration> requestHandlers(
        ChannelRegistration channel) {
        Map<String, ChannelRequestHandlerRegistration> handlers = new HashMap<>();
        for (ZLinkScannedHandler handler : matching(
            channel,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.REQUEST)) {
            handlers.put(handler.packetName(), new ChannelRequestHandlerRegistration(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.messageType(),
                handler.replyType(),
                handler.packetName()));
        }
        for (ChannelRequestHandlerRegistration handler : channel.requestHandlers()) {
            handlers.put(handler.packetName(), handler);
        }
        return Map.copyOf(handlers);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    Map<String, ChannelSendHandlerRegistration> sendHandlers(ChannelRegistration channel) {
        Map<String, ChannelSendHandlerRegistration> handlers = new HashMap<>();
        for (ZLinkScannedHandler handler : matching(
            channel,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.SEND)) {
            handlers.put(handler.packetName(), new ChannelSendHandlerRegistration(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.messageType(),
                handler.packetName()));
        }
        for (ChannelSendHandlerRegistration handler : channel.sendHandlers()) {
            handlers.put(handler.packetName(), handler);
        }
        return Map.copyOf(handlers);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    Map<String, ChannelPublishHandlerRegistration> publishHandlers(ChannelRegistration channel) {
        Map<String, ChannelPublishHandlerRegistration> handlers = new HashMap<>();
        for (ZLinkScannedHandler handler : matching(
            channel,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.PUBLISH)) {
            handlers.put(handler.packetName(), new ChannelPublishHandlerRegistration(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.messageType(),
                handler.packetName()));
        }
        for (ChannelPublishHandlerRegistration handler : channel.publishHandlers()) {
            handlers.put(handler.packetName(), handler);
        }
        return Map.copyOf(handlers);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    Map<String, ChannelRouteRequestHandlerRegistration> routeRequestHandlers(
        ChannelRegistration channel) {
        Map<String, ChannelRouteRequestHandlerRegistration> handlers = new HashMap<>();
        for (ZLinkScannedHandler handler : matching(
            channel,
            ZLinkScannedHandlerSurface.ROUTE,
            ZLinkScannedHandlerKind.REQUEST)) {
            handlers.put(handler.packetName(), new ChannelRouteRequestHandlerRegistration(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.messageType(),
                handler.replyType(),
                handler.packetName()));
        }
        for (ChannelRouteRequestHandlerRegistration handler : channel.routeRequestHandlers()) {
            handlers.put(handler.packetName(), handler);
        }
        return Map.copyOf(handlers);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    Map<String, ChannelRouteSendHandlerRegistration> routeSendHandlers(
        ChannelRegistration channel) {
        Map<String, ChannelRouteSendHandlerRegistration> handlers = new HashMap<>();
        for (ZLinkScannedHandler handler : matching(
            channel,
            ZLinkScannedHandlerSurface.ROUTE,
            ZLinkScannedHandlerKind.SEND)) {
            handlers.put(handler.packetName(), new ChannelRouteSendHandlerRegistration(
                handler.handlerType(),
                handler.handlerMethod(),
                handler.messageType(),
                handler.packetName()));
        }
        for (ChannelRouteSendHandlerRegistration handler : channel.routeSendHandlers()) {
            handlers.put(handler.packetName(), handler);
        }
        return Map.copyOf(handlers);
    }

    private List<ZLinkScannedHandler> matching(
        ChannelRegistration channel,
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind) {
        return scannedHandlers.matching(Set.copyOf(channel.handlerGroups()), surface, kind);
    }
}
