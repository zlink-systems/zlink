package systems.zlink.bench.withgrpc.client;

import com.google.protobuf.UnsafeByteOperations;

import org.springframework.context.annotation.*;

import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.BenchContract;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.*;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spring.*;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.Flow;

/** Loaded beside an unmodified Framework artifact; binding and fixture SPI are shared. */
public final class RequestBackpressureFramework implements RequestBackpressureProbe.Arm {
    static final RoutingId SERVER = RoutingId.from(BenchContract.SERVER_ROUTING_ID);
    static String endpoint;
    final String name;
    final AnnotationConfigApplicationContext server, client;
    final ZLinkRouteClient route;

    public RequestBackpressureFramework(String label) throws Exception {
        name = label;
        try (var socket = new ServerSocket(0)) {
            endpoint = "tcp://127.0.0.1:" + socket.getLocalPort();
        }
        var startedServer = new AnnotationConfigApplicationContext(ServerConfig.class);
        AnnotationConfigApplicationContext startedClient = null;
        try {
            startedClient = new AnnotationConfigApplicationContext(ClientConfig.class);
            ready(startedClient);
            ready(startedServer);
            route = startedClient.getBean(ZLinkRouteClient.class);
        } catch (Throwable error) {
            try {
                if (startedClient != null) startedClient.close();
            } finally {
                startedServer.close();
            }
            throw error;
        }
        server = startedServer;
        client = startedClient;
    }

    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    public static class ServerConfig {
        @Bean
        ZLinkFrameworkConfigurer config() {
            return o -> {
                o.codecs().use(ZLinkProtobufCodec.defaultCodec());
                o.addRouteMesh(BenchContract.MESH_NAME)
                        .listen(endpoint)
                        .setRoutingId(SERVER)
                        .addRouteRequestHandler(Echo.class, BenchPayload.class, BenchPayload.class);
            };
        }

        @Bean
        Echo echo() {
            return new Echo();
        }
    }

    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    public static class ClientConfig {
        @Bean
        ZLinkFrameworkConfigurer config() {
            return o -> {
                o.codecs().use(ZLinkProtobufCodec.defaultCodec());
                var m =
                        o.addRouteMesh(BenchContract.MESH_NAME)
                                .listen("tcp://127.0.0.1:0")
                                .setRoutingId(RoutingId.from(BenchContract.CLIENT_ROUTING_ID));
                m.channelName(BenchContract.CHANNEL_NAME).client();
                m.peerConnections().connect(SERVER, endpoint);
            };
        }
    }

    public static final class Echo implements ZLinkRouteRequestHandler<BenchPayload, BenchPayload> {
        public CompletionStage<BenchPayload> handle(BenchPayload p, ZLinkRouteMessageContext c) {
            return CompletableFuture.completedFuture(p);
        }
    }

    static void ready(AnnotationConfigApplicationContext context) throws Exception {
        var completed = new CompletableFuture<Void>();
        context.getBean(ZLinkRouteMeshRuntime.class)
                .observe(BenchContract.MESH_NAME, 16)
                .subscribe(
                        new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                            Flow.Subscription subscription;

                            public void onSubscribe(Flow.Subscription s) {
                                subscription = s;
                                s.request(Long.MAX_VALUE);
                            }

                            public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> s) {
                                if (s.status().readyPeerCount() > 0) {
                                    completed.complete(null);
                                    subscription.cancel();
                                }
                            }

                            public void onError(Throwable t) {
                                completed.completeExceptionally(t);
                            }

                            public void onComplete() {}
                        });
        completed.get(RequestBackpressureProbe.TIMEOUT.toNanos(), TimeUnit.NANOSECONDS);
    }

    public String name() {
        return name;
    }

    public void tick() {}

    public String pressure() {
        var target =
                server.getBean(ZLinkFrameworkRuntime.class)
                        .status()
                        .capacity()
                        .applicationJobQueue();
        var source =
                client.getBean(ZLinkFrameworkRuntime.class)
                        .status()
                        .capacity()
                        .applicationJobQueue();
        return "target_pressure="
                + target.pressureState()
                + " target_queued="
                + target.queuedApplicationJobs()
                + " target_permits="
                + target.permitsInUse()
                + " target_queue_wait_count="
                + target.capacityWaitCount()
                + " source_pressure="
                + source.pressureState()
                + " source_queued="
                + source.queuedApplicationJobs()
                + " source_permits="
                + source.permitsInUse();
    }

    public BenchRequestOperation.Submission submit(int bytes, byte phase, long sequence) {
        var payload =
                BenchPayload.newBuilder()
                        .setBody(
                                UnsafeByteOperations.unsafeWrap(
                                        BenchMetricHeader.createPayload(
                                                bytes,
                                                RequestBackpressureProbe.RUN_ID,
                                                phase,
                                                sequence)))
                        .build();
        var call =
                route.requestToNode(BenchContract.MESH_NAME, SERVER, payload)
                        .timeout(RequestBackpressureProbe.TIMEOUT)
                        .submit(BenchPayload.class);
        var reply =
                call.toCompletableFuture()
                        .thenAccept(
                                p -> {
                                    if (!BenchMetricHeader.isExpected(
                                            BenchMetricHeader.decode(
                                                    p.getBody().asReadOnlyByteBuffer()),
                                            RequestBackpressureProbe.RUN_ID,
                                            phase,
                                            bytes,
                                            sequence))
                                        throw new AssertionError("framework reply header");
                                });
        return new BenchRequestOperation.Submission(call.admitted().toCompletableFuture(), reply);
    }

    public boolean await(CompletableFuture<Void> f, Duration duration) throws Exception {
        try {
            f.get(duration.toNanos(), TimeUnit.NANOSECONDS);
            return true;
        } catch (TimeoutException e) {
            return false;
        }
    }

    public void close() {
        try {
            client.close();
        } finally {
            server.close();
        }
    }
}
