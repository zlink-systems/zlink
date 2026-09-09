/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import com.google.protobuf.ByteString;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.BenchContract;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

/**
 * {@code zlink-framework-java} client: RouteMesh channel messaging.
 *
 * <p>spec section 8.1: {@code zlink-framework-core} through its public host, the Spring
 * Boot starter, with the protobuf codec from {@code zlink-framework-codec-protobuf}.
 * No internal package is used (G4). The host is stood up once, outside every measured
 * window.
 */
public final class FrameworkStack implements AutoCloseable {
    private final ConfigurableApplicationContext context;
    private final ZLinkRouteClient route;
    private final int runId;
    private final Duration timeout;

    private FrameworkStack(
        ConfigurableApplicationContext context, ZLinkRouteClient route,
        int runId, Duration timeout) {
        this.context = context;
        this.route = route;
        this.runId = runId;
        this.timeout = timeout;
    }

    public static FrameworkStack create(BenchOptions options) {
        ConfigurableApplicationContext context =
            new SpringApplicationBuilder(BenchClientFrameworkApp.class)
                .web(WebApplicationType.NONE)
                .bannerMode(org.springframework.boot.Banner.Mode.OFF)
                .initializers(applicationContext -> applicationContext.getBeanFactory()
                    .registerSingleton("benchPeerEndpoint",
                        new PeerEndpoint(options.zlinkEndpoint)))
                .run();
        ZLinkRouteClient route = context.getBean(ZLinkRouteClient.class);
        return new FrameworkStack(context, route, options.runId,
            Duration.ofMillis(options.requestTimeoutMs));
    }

    public BenchOperation request() {
        return (payloadSize, phase, sequence) -> {
            BenchPayload message = payload(payloadSize, phase, sequence);
            return route.requestToChannel(BenchContract.CHANNEL_NAME, message)
                .timeout(timeout)
                .submit(BenchPayload.class)
                .toCompletableFuture()
                .thenAccept(reply -> {
                    BenchMetricHeader.Decoded decoded = reply == null ? null
                        : BenchMetricHeader.decode(reply.getBody().asReadOnlyByteBuffer());
                    if (!BenchMetricHeader.isExpected(
                        decoded, runId, phase, payloadSize, sequence)) {
                        throw new IllegalStateException("framework reply header mismatch");
                    }
                });
        };
    }

    public BenchOperation send() {
        return (payloadSize, phase, sequence) -> {
            BenchPayload message = payload(payloadSize, phase, sequence);
            return route.sendToChannel(BenchContract.CHANNEL_NAME, message)
                .submit()
                .toCompletableFuture()
                .thenApply(ignored -> (Void) null);
        };
    }

    private BenchPayload payload(int payloadSize, byte phase, long sequence) {
        return BenchPayload.newBuilder()
            .setBody(ByteString.copyFrom(
                BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence)))
            .build();
    }

    @Override
    public void close() {
        context.close();
    }

    public record PeerEndpoint(String value) {
    }

    @EnableZLinkFramework
    @SpringBootApplication(proxyBeanMethods = false)
    public static class BenchClientFrameworkApp {
        @Bean
        ZLinkFrameworkConfigurer benchClientFramework(PeerEndpoint endpoint) {
            return options -> {
                options.codecs().use(ZLinkProtobufCodec.defaultCodec());
                if (System.getenv("BENCH_FLOW_TRACE") != null) {
                    options.configureDispatch().messageFlow(
                        systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
                }
                var mesh = options.addRouteMesh(BenchContract.MESH_NAME)
                    .listen("tcp://127.0.0.1:0")
                    .setRoutingId(RoutingId.from(BenchContract.CLIENT_ROUTING_ID));
                mesh.channelName(BenchContract.CHANNEL_NAME).client();
                // spec section 3: manual endpoint connection, no location store.
                mesh.peerConnections().connect(
                    RoutingId.from(BenchContract.SERVER_ROUTING_ID), endpoint.value());
            };
        }
    }
}
