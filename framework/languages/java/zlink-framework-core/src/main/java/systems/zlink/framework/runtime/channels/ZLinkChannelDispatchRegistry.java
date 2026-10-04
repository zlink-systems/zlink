package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.stream.Stream;

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
    private final Map<String, ZLinkSerialExecutionQueue> clientServerQueues = new HashMap<>();
    private final Map<String, ZLinkSerialExecutionQueue> publishQueues = new HashMap<>();
    private final Map<String, ZLinkSerialExecutionQueue> routeQueues = new HashMap<>();

    ZLinkChannelDispatchRegistry(Executor executor) {
        this.executor = Objects.requireNonNull(executor, "executor");
    }

    void registerClientServer(
            String channelName,
            Map<String, ChannelSendHandlerRegistration> sends,
            Map<String, ChannelRequestHandlerRegistration> requests) {
        sendHandlers.put(channelName, sends);
        requestHandlers.put(channelName, requests);
        clientServerQueues.put(
                channelName,
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.generic()));
    }

    void registerFanout(
            String channelName, Map<String, ChannelPublishHandlerRegistration> publishes) {
        publishHandlers.put(channelName, publishes);
        publishQueues.put(
                channelName,
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.generic()));
    }

    void registerRoute(
            String channelName,
            Map<String, ChannelRouteSendHandlerRegistration> sends,
            Map<String, ChannelRouteRequestHandlerRegistration> requests) {
        routeSendHandlers.put(channelName, sends);
        routeRequestHandlers.put(channelName, requests);
        routeQueues.put(
                channelName,
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.generic()));
    }

    void registerInternalRequest(
            String packetName, ZLinkChannelRuntime.RouteInternalRequestHandler handler) {
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
            String channelName, String packetName) {
        return routeRequestHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ChannelRouteSendHandlerRegistration routeSendHandler(String channelName, String packetName) {
        return routeSendHandlers.getOrDefault(channelName, Map.of()).get(packetName);
    }

    ZLinkChannelRuntime.RouteInternalRequestHandler internalRequest(String packetName) {
        return internalRequests.get(packetName);
    }

    ZLinkSerialExecutionQueue clientServerQueue(String channelName) {
        return clientServerQueues.get(channelName);
    }

    ZLinkSerialExecutionQueue publishQueue(String channelName) {
        return publishQueues.get(channelName);
    }

    ZLinkSerialExecutionQueue routeQueue(String channelName) {
        return routeQueues.get(channelName);
    }

    CompletionStage<Void> awaitQuiescence() {
        List<CompletableFuture<Void>> waiters =
                queues().map(queue -> queue.awaitQuiescence().toCompletableFuture()).toList();
        return CompletableFuture.allOf(waiters.toArray(CompletableFuture[]::new));
    }

    void sealClosingAdmission() {
        queues().forEach(ZLinkSerialExecutionQueue::sealClosingAdmission);
    }

    private Stream<ZLinkSerialExecutionQueue> queues() {
        return Stream.of(clientServerQueues, publishQueues, routeQueues)
                .flatMap(map -> map.values().stream());
    }
}
