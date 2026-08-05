package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

final class ZLinkChannelHandlerFilterContractTest {
    @Test
    void exposesFiveDispatchKindsAndTopologyMeshNames() {
        Probe probe = new Probe();
        ZLinkChannelHandlerInvoker routeMesh = invoker(probe, "mesh-a");
        ZLinkChannelHandlerInvoker clientServer = invoker(probe, null);
        Message payload = text("request");

        routeMesh.invokeSendHandler(
                "channel-a",
                new ChannelSendHandlerRegistration(
                    SendHandler.class, String.class, "send"),
                payload)
            .toCompletableFuture().join();
        routeMesh.invokeRequestHandler(
                "channel-a",
                new ChannelRequestHandlerRegistration(
                    RequestHandler.class, String.class, String.class, "request"),
                payload)
            .toCompletableFuture().join().close();
        routeMesh.invokeRouteSendHandler(
                null,
                new ChannelRouteSendHandlerRegistration(
                    RouteSendHandler.class, String.class, "node-send"),
                RoutingId.from("source"),
                payload)
            .toCompletableFuture().join();
        routeMesh.invokeRouteRequestHandler(
                null,
                new ChannelRouteRequestHandlerRegistration(
                    RouteRequestHandler.class,
                    String.class,
                    String.class,
                    "node-request"),
                RoutingId.from("source"),
                payload)
            .toCompletableFuture().join().close();
        routeMesh.invokePublishHandler(
                "fanout-a",
                new ChannelPublishHandlerRegistration(
                    PublishHandler.class, String.class, "publish"),
                "topic-a",
                payload)
            .toCompletableFuture().join();
        clientServer.invokeRequestHandler(
                "channel-b",
                new ChannelRequestHandlerRegistration(
                    RequestHandler.class, String.class, String.class, "request"),
                payload)
            .toCompletableFuture().join().close();
        payload.close();

        assertEquals(
            List.of(
                ZLinkHandlerDispatchKind.CHANNEL_SEND,
                ZLinkHandlerDispatchKind.CHANNEL_REQUEST,
                ZLinkHandlerDispatchKind.NODE_DIRECT_SEND,
                ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST,
                ZLinkHandlerDispatchKind.CLASSIC_FANOUT,
                ZLinkHandlerDispatchKind.CHANNEL_REQUEST),
            probe.kinds);
        assertEquals(
            List.of("mesh-a", "mesh-a", "mesh-a", "mesh-a", "", ""),
            probe.meshNames);
    }

    @Test
    void requestShortCircuitIsRejectedWithoutSerializingFilterValue() {
        Probe probe = new Probe();
        probe.stopRequests = true;
        CompletionException failure = org.junit.jupiter.api.Assertions.assertThrows(
            CompletionException.class,
            () -> invoker(probe, null)
                .invokeRequestHandler(
                    "channel-a",
                    new ChannelRequestHandlerRegistration(
                        RequestHandler.class,
                        String.class,
                        String.class,
                        "request"),
                    text("request"))
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException rejected = assertInstanceOf(
            ZLinkFrameworkException.class,
            failure.getCause());
        assertEquals(ZLinkFrameworkErrorKind.REJECTED, rejected.kind());
        assertEquals(0, probe.handlerCalls);
    }

    @Test
    void legacyRouteDispatcherUsesRouteNameAsMeshName() {
        Probe probe = new Probe();
        Message payload = text("request");
        invoker(probe, null).invokeRouteRequestHandler(
                "legacy-mesh",
                new ChannelRouteRequestHandlerRegistration(
                    RouteRequestHandler.class,
                    String.class,
                    String.class,
                    "node-request"),
                RoutingId.from("source"),
                payload)
            .toCompletableFuture().join().close();
        payload.close();

        assertEquals(
            List.of(ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST),
            probe.kinds);
        assertEquals(List.of("legacy-mesh"), probe.meshNames);
    }

    @Test
    void fanoutCreatesAndDisposesOneFilterScopePerHandlerDispatch() {
        Probe probe = new Probe();
        ZLinkChannelHandlerInvoker invoker = invoker(probe, null);
        Message payload = text("event");
        ChannelPublishHandlerRegistration registration =
            new ChannelPublishHandlerRegistration(
                PublishHandler.class, String.class, "publish");

        invoker.invokePublishHandler(
            "fanout-a", registration, "topic-a", payload)
            .toCompletableFuture().join();
        invoker.invokePublishHandler(
            "fanout-a", registration, "topic-a", payload)
            .toCompletableFuture().join();
        payload.close();

        assertEquals(2, probe.filterCreates);
        assertEquals(2, probe.filterCloses);
        assertEquals(2, probe.handlerCalls);
    }

    private static ZLinkChannelHandlerInvoker invoker(
        Probe probe,
        String meshName) {
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        ZLinkStringMessageSerializer serializer =
            new ZLinkStringMessageSerializer();
        codecs.addSerializer("text/plain", serializer);
        ZLinkHandlerActivator activator = new ZLinkHandlerActivator() {
            @Override
            public Object create(Class<?> type) {
                if (type == CapturingFilter.class) {
                    probe.filterCreates++;
                    return new CapturingFilter(probe);
                }
                if (type == SendHandler.class) return new SendHandler(probe);
                if (type == RequestHandler.class) return new RequestHandler(probe);
                if (type == RouteSendHandler.class) return new RouteSendHandler(probe);
                if (type == RouteRequestHandler.class) return new RouteRequestHandler(probe);
                if (type == PublishHandler.class) return new PublishHandler(probe);
                throw new IllegalArgumentException("unexpected type: " + type);
            }
        };
        return new ZLinkChannelHandlerInvoker(
            serializer,
            codecs,
            activator,
            Runnable::run,
            List.of(),
            List.of(CapturingFilter.class),
            meshName);
    }

    private static Message text(String value) {
        return Message.from(value.getBytes(StandardCharsets.UTF_8));
    }

    private static final class Probe {
        private final List<ZLinkHandlerDispatchKind> kinds = new ArrayList<>();
        private final List<String> meshNames = new ArrayList<>();
        private int filterCreates;
        private int filterCloses;
        private int handlerCalls;
        private boolean stopRequests;
    }

    private static final class CapturingFilter
        implements ZLinkHandlerFilter, AutoCloseable {
        private final Probe probe;

        private CapturingFilter(Probe probe) {
            this.probe = probe;
        }

        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            probe.kinds.add(context.dispatchKind());
            probe.meshNames.add(context.meshName().orElse(""));
            if (probe.stopRequests
                && (context.dispatchKind()
                    == ZLinkHandlerDispatchKind.CHANNEL_REQUEST
                    || context.dispatchKind()
                    == ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST)) {
                @SuppressWarnings("unchecked")
                T ignoredReplacement = (T) "filter-reply";
                return CompletableFuture.completedFuture(ignoredReplacement);
            }
            return next.invoke();
        }

        @Override
        public void close() {
            probe.filterCloses++;
        }
    }

    private record SendHandler(Probe probe)
        implements ZLinkSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(
            String message,
            systems.zlink.framework.ZLinkMessageContext context) {
            probe.handlerCalls++;
            return CompletableFuture.completedFuture(null);
        }
    }

    private record RequestHandler(Probe probe)
        implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(
            String request,
            systems.zlink.framework.ZLinkMessageContext context) {
            probe.handlerCalls++;
            return CompletableFuture.completedFuture(request);
        }
    }

    private record RouteSendHandler(Probe probe)
        implements ZLinkRouteSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkRouteMessageContext context) {
            probe.handlerCalls++;
            return CompletableFuture.completedFuture(null);
        }
    }

    private record RouteRequestHandler(Probe probe)
        implements ZLinkRouteRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(
            String request,
            ZLinkRouteMessageContext context) {
            probe.handlerCalls++;
            return CompletableFuture.completedFuture(request);
        }
    }

    private record PublishHandler(Probe probe)
        implements ZLinkFanoutHandler<String> {
        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkPublishMessageContext context) {
            probe.handlerCalls++;
            return CompletableFuture.completedFuture(null);
        }
    }
}
