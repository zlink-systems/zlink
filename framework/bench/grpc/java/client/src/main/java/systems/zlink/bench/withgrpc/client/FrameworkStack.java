/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import com.google.protobuf.UnsafeByteOperations;

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
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

import java.time.Duration;

/**
 * {@code zlink-framework-java} client: RouteMesh messaging to an explicit node RID.
 *
 * <p>spec section 8.1: {@code zlink-framework-core} through its public host, the Spring Boot
 * starter, with the protobuf codec from {@code zlink-framework-codec-protobuf}. No internal package
 * is used (G4). The host is stood up once, outside every measured window.
 */
public final class FrameworkStack implements AutoCloseable {
    private static final RoutingId SERVER = RoutingId.from(BenchContract.SERVER_ROUTING_ID);
    private static final ZLinkChannelEnvelope.PreparedNames CHANNEL_NAMES =
            new ZLinkChannelEnvelope.PreparedNames(BenchContract.CHANNEL_NAME, null, null);
    private static final ZLinkChannelEnvelope.PreparedNames MESSAGE_NAMES =
            new ZLinkChannelEnvelope.PreparedNames(
                    null, ladderMessageName(), ladderContentType());

    private final ConfigurableApplicationContext context;
    private final ZLinkRouteClient route;
    private final systems.zlink.contracts.core.Context ladderContext;
    private final RawStack ladderBinding;
    private final int runId;
    private final Duration timeout;

    private FrameworkStack(
            ConfigurableApplicationContext context,
            ZLinkRouteClient route,
            systems.zlink.contracts.core.Context ladderContext,
            RawStack ladderBinding,
            int runId,
            Duration timeout) {
        this.context = context;
        this.route = route;
        this.ladderContext = ladderContext;
        this.ladderBinding = ladderBinding;
        this.runId = runId;
        this.timeout = timeout;
    }

    public static FrameworkStack create(BenchOptions options) {
        if (ladderMode()) {
            var bindingContext = systems.zlink.contracts.core.Zlink.createContext();
            var binding = RawStack.create(
                    bindingContext,
                    options,
                    "bench-source-" + ProcessHandle.current().pid(),
                    BenchContract.SERVER_ROUTING_ID,
                    options.targetEndpoint);
            return new FrameworkStack(null, null, bindingContext, binding,
                    options.runId, Duration.ofMillis(options.requestTimeoutMs));
        }
        ConfigurableApplicationContext context =
                new SpringApplicationBuilder(BenchClientFrameworkApp.class)
                        .web(WebApplicationType.NONE)
                        .bannerMode(org.springframework.boot.Banner.Mode.OFF)
                        .initializers(
                                applicationContext ->
                                        applicationContext
                                                .getBeanFactory()
                                                .registerSingleton(
                                                        "benchPeerEndpoint",
                                                        new PeerEndpoint(options.targetEndpoint)))
                        .run();
        ZLinkRouteClient route = context.getBean(ZLinkRouteClient.class);
        return new FrameworkStack(
                context, route, null, null,
                options.runId, Duration.ofMillis(options.requestTimeoutMs));
    }

    public BenchRequestOperation request() {
        return (payloadSize, phase, sequence) -> {
            BenchPayload message = payload(payloadSize, phase, sequence);
            var submitted =
                    route.requestToNode(BenchContract.MESH_NAME, SERVER, message)
                            .timeout(timeout)
                            .submit(BenchPayload.class);
            var replyCompletion =
                    submitted
                            .toCompletableFuture()
                            .thenAccept(
                                    reply -> {
                                        BenchMetricHeader.Decoded decoded =
                                                reply == null
                                                        ? null
                                                        : BenchMetricHeader.decode(
                                                                reply.getBody()
                                                                        .asReadOnlyByteBuffer());
                                        if (!BenchMetricHeader.isExpected(
                                                decoded, runId, phase, payloadSize, sequence)) {
                                            throw new IllegalStateException(
                                                    "framework reply header mismatch");
                                        }
                                    });
            return new BenchRequestOperation.Submission(
                    submitted.admitted().toCompletableFuture(), replyCompletion);
        };
    }

    public BenchOperation send() {
        return (payloadSize, phase, sequence) -> {
            BenchPayload message = payload(payloadSize, phase, sequence);
            return route.sendToNode(BenchContract.MESH_NAME, SERVER, message)
                    .submit()
                    .toCompletableFuture()
                    .thenApply(ignored -> (Void) null);
        };
    }

    /** Exposes the binding-shaped L0 operation to the existing raw benchmark driver. */
    public BenchOperation ladderOperation(boolean send) {
        if (ladderBinding == null) {
            throw new IllegalStateException("L0 operation requested outside ladder mode");
        }
        if (System.getenv("ZLINK_BENCH_LADDER_L4") != null) {
            return send
                    ? ladderBinding.sendWithTypedJsonServiceWireEnvelope(
                            ZLinkChannelEnvelope.KIND_COMMAND, FrameworkStack::envelopeParts)
                    : ladderBinding.requestWithTypedJsonServiceWireEnvelope(
                            ZLinkChannelEnvelope.KIND_REQUEST, FrameworkStack::envelopeParts);
        }
        if (System.getenv("ZLINK_BENCH_LADDER_L3") != null) {
            return send
                    ? ladderBinding.sendWithServiceWireEnvelope(
                            ZLinkChannelEnvelope.KIND_COMMAND, FrameworkStack::envelopeParts)
                    : ladderBinding.requestWithServiceWireEnvelope(
                            ZLinkChannelEnvelope.KIND_REQUEST, FrameworkStack::envelopeParts);
        }
        if (System.getenv("ZLINK_BENCH_LADDER_L2") != null) {
            return send
                    ? ladderBinding.sendWithEnvelope(
                            ZLinkChannelEnvelope.KIND_COMMAND, FrameworkStack::envelopeParts)
                    : ladderBinding.requestWithEnvelope(
                            ZLinkChannelEnvelope.KIND_REQUEST, FrameworkStack::envelopeParts);
        }
        return send ? ladderBinding.send() : ladderBinding.request();
    }

    private static java.util.List<systems.zlink.contracts.messaging.Message> envelopeParts(
            int kind,
            systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView body) {
        var header =
                ZLinkChannelEnvelope.create(
                        kind,
                        BenchContract.CHANNEL_NAME,
                        ladderMessageName(),
                        ladderContentType(),
                        null,
                        java.util.Map.of(),
                        null);
        return ZLinkChannelEnvelope.encode(
                header, body, CHANNEL_NAMES, MESSAGE_NAMES, MESSAGE_NAMES);
    }

    private BenchPayload payload(int payloadSize, byte phase, long sequence) {
        return BenchPayload.newBuilder()
                .setBody(
                        UnsafeByteOperations.unsafeWrap(
                                BenchMetricHeader.createPayload(
                                        payloadSize, runId, phase, sequence)))
                .build();
    }

    @Override
    public void close() {
        if (context != null) {
            context.close();
        }
        if (ladderBinding != null) {
            ladderBinding.close();
            ladderContext.close();
        }
    }

    public record PeerEndpoint(String value) {}

    private static boolean ladderMode() {
        return System.getenv("ZLINK_BENCH_LADDER_L0") != null
                || System.getenv("ZLINK_BENCH_LADDER_L1") != null
                || System.getenv("ZLINK_BENCH_LADDER_L2") != null
                || System.getenv("ZLINK_BENCH_LADDER_L3") != null
                || System.getenv("ZLINK_BENCH_LADDER_L4") != null;
    }

    private static String ladderMessageName() {
        return System.getenv("ZLINK_BENCH_LADDER_L4") != null
                ? "BenchTypedPayload"
                : "BenchPayload";
    }

    private static String ladderContentType() {
        return System.getenv("ZLINK_BENCH_LADDER_L4") != null
                ? "application/json"
                : "application/x-protobuf";
    }

    @EnableZLinkFramework
    @SpringBootApplication(proxyBeanMethods = false)
    public static class BenchClientFrameworkApp {
        @Bean
        ZLinkFrameworkConfigurer benchClientFramework(PeerEndpoint endpoint) {
            return options -> {
                options.codecs().use(ZLinkProtobufCodec.defaultCodec());
                if (System.getenv("BENCH_FLOW_TRACE") != null) {
                    options.configureDispatch()
                            .messageFlow(
                                    systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
                                            .NORMAL);
                }
                var mesh =
                        options.addRouteMesh(BenchContract.MESH_NAME)
                                .listen("tcp://127.0.0.1:0")
                                .setRoutingId(RoutingId.from(BenchContract.CLIENT_ROUTING_ID));
                mesh.channelName(BenchContract.CHANNEL_NAME).client();
                // spec section 3: manual endpoint connection, no location store.
                mesh.peerConnections().connect(SERVER, endpoint.value());
            };
        }
    }
}
