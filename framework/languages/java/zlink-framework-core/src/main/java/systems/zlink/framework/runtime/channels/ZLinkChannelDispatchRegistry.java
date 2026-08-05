package systems.zlink.framework.runtime.channels;

import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

final class ZLinkChannelDispatchRegistry {
    private final Executor executor;
    private final Map<String, Map<String, ChannelRequestHandlerRegistration>> requestHandlers =
        new HashMap<>();
    private final Map<String, Map<String, ChannelSendHandlerRegistration>> sendHandlers =
        new HashMap<>();
    private final Map<String, Map<String, ChannelPublishHandlerRegistration>> publishHandlers =
        new HashMap<>();
    private final Map<String, Map<String, ChannelRouteRequestHandlerRegistration>>
        routeRequestHandlers = new HashMap<>();
    private final Map<String, Map<String, ChannelRouteSendHandlerRegistration>> routeSendHandlers =
        new HashMap<>();
    private final Map<String, ZLinkChannelRuntime.RouteInternalRequestHandler> internalRequests =
        new HashMap<>();
    private final Map<String, ZLinkAsyncSerialQueue> sendQueues = new HashMap<>();
    private final Map<String, ZLinkAsyncSerialQueue> requestQueues = new HashMap<>();
    private final Map<String, ZLinkAsyncSerialQueue> publishQueues = new HashMap<>();
    private final Map<String, ZLinkAsyncSerialQueue> routeRequestQueues = new HashMap<>();
    private final Map<String, ZLinkAsyncSerialQueue> routeSendQueues = new HashMap<>();

    ZLinkChannelDispatchRegistry(Executor executor) {
        this.executor = Objects.requireNonNull(executor, "executor");
    }

    void registerClientServer(
        String channelName,
        Map<String, ChannelSendHandlerRegistration> sends,
        Map<String, ChannelRequestHandlerRegistration> requests) {
        sendHandlers.put(channelName, sends);
        requestHandlers.put(channelName, requests);
        sendQueues.put(channelName, new ZLinkAsyncSerialQueue(executor, false));
        requestQueues.put(channelName, new ZLinkAsyncSerialQueue(executor, false));
    }

    void registerFanout(
        String channelName,
        Map<String, ChannelPublishHandlerRegistration> publishes) {
        publishHandlers.put(channelName, publishes);
        publishQueues.put(channelName, new ZLinkAsyncSerialQueue(executor, false));
    }

    void registerRoute(
        String channelName,
        Map<String, ChannelRouteSendHandlerRegistration> sends,
        Map<String, ChannelRouteRequestHandlerRegistration> requests) {
        routeSendHandlers.put(channelName, sends);
        routeRequestHandlers.put(channelName, requests);
        routeSendQueues.put(channelName, new ZLinkAsyncSerialQueue(executor, false));
        routeRequestQueues.put(channelName, new ZLinkAsyncSerialQueue(executor, false));
    }

    void registerInternalRequest(
        String packetName,
        ZLinkChannelRuntime.RouteInternalRequestHandler handler) {
        if (internalRequests.putIfAbsent(packetName, handler) != null) {
            throw new ZLinkConfigurationException(
                "duplicate internal route packet handler: " + packetName);
        }
    }

    ChannelRequestHandlerRegistration requestHandler(String channelName, String packetName) {
        return requestHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ChannelSendHandlerRegistration sendHandler(String channelName, String packetName) {
        return sendHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ChannelPublishHandlerRegistration publishHandler(String channelName, String packetName) {
        return publishHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ChannelRouteRequestHandlerRegistration routeRequestHandler(
        String channelName,
        String packetName) {
        return routeRequestHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ChannelRouteSendHandlerRegistration routeSendHandler(
        String channelName,
        String packetName) {
        return routeSendHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ZLinkChannelRuntime.RouteInternalRequestHandler internalRequest(String packetName) {
        return internalRequests.get(packetName);
    }

    ZLinkAsyncSerialQueue requestQueue(String channelName) {
        return requestQueues.get(channelName);
    }

    ZLinkAsyncSerialQueue sendQueue(String channelName) {
        return sendQueues.get(channelName);
    }

    ZLinkAsyncSerialQueue publishQueue(String channelName) {
        return publishQueues.get(channelName);
    }

    ZLinkAsyncSerialQueue routeRequestQueue(String channelName) {
        return routeRequestQueues.get(channelName);
    }

    ZLinkAsyncSerialQueue routeSendQueue(String channelName) {
        return routeSendQueues.get(channelName);
    }
}
