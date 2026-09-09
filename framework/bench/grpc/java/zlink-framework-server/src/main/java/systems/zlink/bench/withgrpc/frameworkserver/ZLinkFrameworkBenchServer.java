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
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

/**
 * {@code zlink-framework-<lang>} server, java row.
 *
 * <p>spec section 1.3: RouteMesh ROUTER&lt;-&gt;ROUTER with a channel request handler and a
 * channel send handler. The host is the Spring Boot starter, which is the public way
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
    private static volatile String listenEndpoint = "tcp://127.0.0.1:5092";

    public static void main(String[] args) {
        String endpoint = Args.value(args, "--endpoint", "tcp://127.0.0.1:5092");
        String metricsUrl = Args.value(args, "--metrics-url", "http://127.0.0.1:5093");
        BenchServerMetrics metrics = METRICS;
        listenEndpoint = endpoint;

        new SpringApplicationBuilder(ZLinkFrameworkBenchServer.class)
            .web(WebApplicationType.NONE)
            .bannerMode(org.springframework.boot.Banner.Mode.OFF)
            .run(args);

        BenchStatsServer.start(metricsUrl, metrics,
            "{\"implementation\":\"zlink-framework-java\","
            + "\"host\":\"zlink-framework-spring-boot-starter\","
            + "\"codec\":\"zlink-framework-codec-protobuf\","
            + "\"endpoint\":\"" + endpoint + "\"}");
        System.err.println("[framework-server] endpoint=" + endpoint
            + " stats=" + metricsUrl);
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
            // spec section 1.3: one RouteMesh channel carrying both a request handler and
            // a send handler, registered for the same message type as in the .NET and
            // node rows so the three language rows stay comparable.
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
    BenchEchoHandler benchEchoHandler() {
        return new BenchEchoHandler();
    }

    @Bean
    BenchCommandHandler benchCommandHandler(BenchServerMetrics metrics) {
        return new BenchCommandHandler(metrics);
    }
}
