/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import systems.zlink.bench.withgrpc.shared.Args;
import systems.zlink.bench.withgrpc.shared.BenchContract;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.bench.withgrpc.shared.BenchStatsServer;
import systems.zlink.bench.withgrpc.shared.MeasurementTiming;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.OptionalLong;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

/**
 * {@code zlink-framework-<lang>} server, java row.
 *
 * <p>RouteMesh ROUTER&lt;-&gt;ROUTER with node-direct request and send handlers.
 * The host is the Spring Boot starter, which is the public way
 * to stand up {@code zlink-framework-core} -- the analogue of .NET's
 * {@code Zlink.Framework.AspNetCore} and node's {@code @zlink-systems/nestjs}. No
 * internal package of the framework is touched (G4).
 */
@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public class ZLinkFrameworkBenchServer {
    // These are handed to the context as @Bean METHODS, not as singletons registered
    // on the bean factory. A registered singleton has an instance but no bean
    // DEFINITION, and the framework's Spring handler factory asks whether each
    // constructor dependency is a prototype -- a question that can only be answered
    // from a definition. A handler that depends on a registered singleton therefore
    // cannot be constructed, and the dispatcher swallows that failure silently: the
    // send is traced received/admitted/dispatched, the sender is told it succeeded,
    // and the handler never runs. This wiring is what makes the send row measurable.
    private static final BenchServerMetrics METRICS = new BenchServerMetrics();
    private static volatile String listenEndpoint = "tcp://127.0.0.1:5254";

    public static void main(String[] args) throws Exception {
        MeasurementTiming.initialize();
        String endpoint = Args.value(args, "--endpoint", "tcp://127.0.0.1:5254");
        String metricsUrl = Args.value(args, "--metrics-url", "http://127.0.0.1:5255");
        BenchServerMetrics metrics = METRICS;
        listenEndpoint = endpoint;

        if (ladderMode()) {
            runLadderL0(endpoint, metricsUrl, metrics);
            return;
        }

        try (var context = new SpringApplicationBuilder(ZLinkFrameworkBenchServer.class)
            .web(WebApplicationType.NONE)
            .bannerMode(org.springframework.boot.Banner.Mode.OFF)
            .run(args)) {

            var stats = BenchStatsServer.start(metricsUrl, metrics,
                "{\"implementation\":\"zlink-framework-java\","
                + "\"host\":\"zlink-framework-spring-boot-starter\","
                + "\"codec\":\"zlink-framework-codec-protobuf\","
                + "\"endpoint\":\"" + endpoint + "\"}");
            try {
                System.err.println("[framework-server] endpoint=" + endpoint
                    + " stats=" + metricsUrl);
                if (System.in.read() < 0) {
                    Thread.currentThread().join();
                }
            } finally {
                stats.stop(0);
            }
        }
    }

    /** Ladder L0/L1: binding-shaped owner loop and direct fixed handler. */
    private static void runLadderL0(
            String endpoint, String metricsUrl, BenchServerMetrics metrics) throws Exception {
        try (Context context = Zlink.createContext();
                RouterSocket router = context.createRouterSocket()) {
            router.setRoutingId(RoutingId.from(BenchContract.SERVER_ROUTING_ID));
            router.options().mandatory(true);
            router.bind(endpoint);
            var stats = BenchStatsServer.start(
                    metricsUrl,
                    metrics,
                    "{\"implementation\":\"zlink-framework-java-ladder\","
                            + "\"shape\":\"binding receive and protobuf reply\","
                            + "\"endpoint\":\"" + endpoint + "\"}");
            Thread owner = new Thread(
                    () -> pumpLadderL0(router, metrics), "bench-framework-ladder-owner");
            owner.setDaemon(true);
            try {
                owner.start();
                System.err.println("[framework-l0] endpoint=" + endpoint
                        + " stats=" + metricsUrl);
                if (System.in.read() < 0) {
                    Thread.currentThread().join();
                }
            } finally {
                context.shutdown();
                owner.join();
                stats.stop(0);
            }
        }
    }

    private static void pumpLadderL0(RouterSocket router, BenchServerMetrics metrics) {
        boolean permitsEnabled = System.getenv("ZLINK_BENCH_LADDER_L1") != null;
        ZLinkApplicationJobQueue permits = permitsEnabled
                ? new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.empty(),
                        ZLinkApplicationJobQueue.productionProcessorCandidates(null))
                : null;
        try (Received received = new Received()) {
            while (true) {
                boolean first = true;
                while (true) {
                    ZLinkApplicationJobQueue.Permit permit = null;
                    try {
                        if (permitsEnabled) {
                            permit = permits.acquireBlocking(
                                    ZLinkApplicationJobQueue.Origin.REMOTE);
                        }
                        boolean receivedMessage = router.recv(
                                received, first ? RecvFlags.NONE : RecvFlags.DONT_WAIT);
                        if (!receivedMessage) {
                            if (permit != null) {
                                permit.close();
                            }
                            break;
                        }
                        first = false;
                        if (permit != null) {
                            permit.handlerStarted();
                        }
                        handleLadderL0(received, metrics);
                    } catch (ZlinkRecvException failure) {
                        if (failure.getResult() == RecvResult.TERMINATED) {
                            return;
                        }
                        if (failure.getResult() != RecvResult.NO_DATA) {
                            metrics.recordError();
                            System.err.println("L0 receive failed: " + failure);
                        }
                        if (failure.getResult() == RecvResult.NO_DATA || first) {
                            break;
                        }
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        return;
                    } catch (RuntimeException failure) {
                        metrics.recordError();
                        System.err.println("L0 owner failed: " + failure);
                        break;
                    } finally {
                        if (permit != null) {
                            permit.close();
                        }
                    }
                }
            }
        } finally {
            if (permits != null) {
                permits.close();
            }
        }
    }

    private static boolean ladderMode() {
        return System.getenv("ZLINK_BENCH_LADDER_L0") != null
                || System.getenv("ZLINK_BENCH_LADDER_L1") != null;
    }

    private static void handleLadderL0(Received received, BenchServerMetrics metrics) {
        var parts = received.parts();
        if (parts.isEmpty()) {
            metrics.recordError();
            return;
        }
        var body = RawWire.decodeBenchPayloadBody(parts.getLast().dataBuffer());
        if (body == null) {
            metrics.recordError();
            return;
        }
        metrics.record(body);
        if (received.replyToken().isPresent()) {
            try (Message header = Message.from(RawWire.RESPONSE_ENVELOPE);
                    Message replyBody = RawWire.encodeBenchPayloadMessage(body)) {
                received.reply().message(header).message(replyBody).submit();
            }
        }
    }

    /** Carries the listen endpoint into the configurer without a system property. */
    public record BenchEndpoint(String value) {
    }

    @Bean
    BenchServerMetrics benchMetrics() {
        return METRICS;
    }

    @Bean
    BenchEndpoint benchEndpoint() {
        return new BenchEndpoint(listenEndpoint);
    }

    @Bean
    ZLinkFrameworkConfigurer benchFramework(BenchEndpoint endpoint) {
        return options -> {
            options.codecs().use(ZLinkProtobufCodec.defaultCodec());
            // A send that finds no handler is dropped silently by default, which
            // would reach the client as a throughput of zero with no explanation.
            // G3 counts what the SERVER received, so the server has to say when it
            // received something it could not dispatch.
            if (System.getenv("BENCH_FLOW_TRACE") != null) {
                options.configureDispatch().messageFlow(
                    systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
            }
            if (System.getenv("BENCH_DISPATCH_PROBE") != null) {
                options.useFilter(BenchDispatchProbeFilter.class);
            }
            options.configureDispatch().unhandled()
                .setSend(ZLinkUnhandledDispatchAction.LOG_AND_DROP);
            var mesh = options.addRouteMesh(BenchContract.MESH_NAME)
                .listen(endpoint.value())
                .setRoutingId(RoutingId.from(BenchContract.SERVER_ROUTING_ID));
            mesh.addRouteRequestHandler(
                    BenchEchoHandler.class,
                    systems.zlink.bench.withgrpc.proto.BenchPayload.class,
                    systems.zlink.bench.withgrpc.proto.BenchPayload.class)
                .addRouteSendHandler(
                    BenchCommandHandler.class,
                    systems.zlink.bench.withgrpc.proto.BenchPayload.class);
            // Kotlin reuses this server and still addresses the channel. Both public
            // namespaces use the same handlers, codec and metrics.
            mesh.channelName(BenchContract.CHANNEL_NAME).server()
                .addRequestHandler(
                    BenchEchoHandler.class,
                    systems.zlink.bench.withgrpc.proto.BenchPayload.class,
                    systems.zlink.bench.withgrpc.proto.BenchPayload.class)
                .addRouteSendHandler(
                    BenchCommandHandler.class,
                    systems.zlink.bench.withgrpc.proto.BenchPayload.class);
        };
    }

    @Bean
    BenchDispatchProbeFilter benchDispatchProbeFilter() {
        return new BenchDispatchProbeFilter();
    }

    @Bean
    BenchEchoHandler benchEchoHandler(BenchServerMetrics metrics) {
        return new BenchEchoHandler(metrics);
    }

    @Bean
    BenchCommandHandler benchCommandHandler(BenchServerMetrics metrics) {
        return new BenchCommandHandler(metrics);
    }
}
