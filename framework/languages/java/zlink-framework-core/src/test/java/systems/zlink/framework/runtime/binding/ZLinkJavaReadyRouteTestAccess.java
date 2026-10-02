package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

/** Exercises the existing Ready owner ingress with the fixture's wire intent. */
public final class ZLinkJavaReadyRouteTestAccess {
    private ZLinkJavaReadyRouteTestAccess() {}

    public static CompletionStage<Void> sendReady(
            ZLinkInternalMeshNode source,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route,
            long sourceGeneration,
            boolean intent,
            List<Message> parts)
            throws ReflectiveOperationException {
        var raw = (ZLinkJavaRawMeshNode) source;
        var portField = ZLinkJavaRawMeshNode.class.getDeclaredField("port");
        var routerField = ZLinkJavaRawMeshNode.class.getDeclaredField("router");
        portField.setAccessible(true);
        routerField.setAccessible(true);
        var port = (ZLinkJavaRawServicePort) portField.get(raw);
        var router = (RouterSocket) routerField.get(raw);
        var header =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                        0,
                        route,
                        intent,
                        sourceGeneration,
                        source.routingId(),
                        null,
                        false,
                        0,
                        0,
                        null);
        return port.send(
                router,
                route.targetNodeRid(),
                List.of(
                        new ZLinkServiceM6BWireCodec().encodeInstanceSpotHeader(header),
                        ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(parts)));
    }

    public static CompletableFuture<Throwable> rejectReadyRequest(
            ZLinkInternalMeshNode node,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route,
            boolean intent,
            AtomicInteger terminalCount) {
        CompletableFuture<Throwable> terminal = new CompletableFuture<>();
        var header =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                        0,
                        route,
                        intent,
                        route.targetNodeGeneration(),
                        node.routingId(),
                        null,
                        true,
                        1,
                        2,
                        3L);
        var wire = new ZLinkServiceM6BWireCodec();
        header = wire.decodeInstanceSpotHeader(wire.encodeInstanceSpotHeader(header));
        boolean accepted =
                ((ZLinkJavaRawSpotNode) node.spotNode())
                        .enqueueRemoteInstanceSpot(
                                node.routingId(),
                                header,
                                new byte[0],
                                List.of(),
                                null,
                                parts -> {
                                    terminalCount.incrementAndGet();
                                    parts.forEach(systems.zlink.contracts.messaging.Message::close);
                                    terminal.complete(
                                            new AssertionError(
                                                    "stale Ready ingress reached reply"));
                                },
                                failure -> {
                                    terminalCount.incrementAndGet();
                                    terminal.complete(failure);
                                });
        if (!accepted)
            terminal.complete(new AssertionError("stale Ready ingress was not classified"));
        return terminal;
    }
}
