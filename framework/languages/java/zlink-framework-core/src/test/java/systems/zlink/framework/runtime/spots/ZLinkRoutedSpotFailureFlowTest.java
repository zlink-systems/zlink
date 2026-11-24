package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.annotation.JsonProperty;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkJsonOptional;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkRoutedSpotFailureFlowTest {
    private static final String MESH = "routed-spot-failure-mesh";
    private static final String SPOT_TYPE = "routed-spot-failure";
    private static final String PACKET = "RoutedFailureNotice";
    private static final long WAIT_SECONDS = 5;

    @Test
    void asynchronousHandlerFailureKeepsInboundFlowAndReportsOnce() throws Exception {
        try (var fixture = new Fixture()) {
            var failed =
                    fixture.receive(
                            """
                    {"text":"fail","sequence":"7"}
                    """);
            await(fixture.instance.handlerStarted);
            assertFalse(fixture.instance.handlerTerminal.isDone());
            fixture.instance.handlerTerminal.completeExceptionally(
                    new IllegalStateException("notice handler failed after start"));
            String error = fixture.error(failed, "handler_exception");
            assertTrue(error.contains("error_message=notice handler failed after start"), error);
            assertFalse(
                    fixture.records(failed).stream()
                            .anyMatch(line -> line.contains("phase=completed")));
            assertTrue(
                    fixture.records(failed).stream()
                            .anyMatch(line -> line.contains("phase=received")));

            var successful =
                    fixture.receive(
                            """
                    {"text":"ok","sequence":"7"}
                    """);
            await(successful.terminal());
            ZLinkTelemetryTestFence.awaitIdle();
            var successRecords = fixture.records(successful);
            assertEquals(
                    1,
                    successRecords.stream()
                            .filter(line -> line.contains("phase=completed"))
                            .count(),
                    successRecords.toString());
            assertFalse(
                    successRecords.stream()
                            .anyMatch(line -> line.contains("zlink.dispatch_error")));
        }
    }

    @Test
    void routedDecodeErrorsRetainCodecDiagnosticsWithoutPayload() throws Exception {
        try (var fixture = new Fixture()) {
            var nullPacket =
                    fixture.receive(
                            """
                    {"text":null,"sequence":"7","secret":"payload-secret"}
                    """);
            String nullError = fixture.error(nullPacket, "decode_error");
            var numericPacket =
                    fixture.receive(
                            """
                    {"text":"payload-secret","sequence":7}
                    """);
            String numericError = fixture.error(numericPacket, "decode_error");
            assertNullDiagnostic(nullError, "text");
            assertTrue(
                    numericError.contains(
                            "error_message=Message '"
                                    + Notice.class.getName()
                                    + "': Property 'sequence' expects a decimal string for a 64-bit integer (framework-json-v1 §5)."),
                    numericError);
            assertFalse(nullError.contains("payload-secret"), nullError);
            assertFalse(numericError.contains("payload-secret"), numericError);
            assertFalse(fixture.instance.handlerStarted.isDone());
        }
    }

    @Test
    void routedDecodeErrorRetainsNestedWirePathWithoutPayload() throws Exception {
        try (var fixture = new Fixture()) {
            var packet =
                    fixture.receive(
                            """
                    {"text":"payload-secret","sequence":"7","detail":{"wire_text":null}}
                    """);
            String error = fixture.error(packet, "decode_error");
            assertNullDiagnostic(error, "detail.wire_text");
            assertFalse(error.contains("payload-secret"), error);
        }
    }

    private static void assertNullDiagnostic(String error, String path) {
        assertTrue(
                error.contains(
                        "error_message=Message '"
                                + Notice.class.getName()
                                + "': Property '"
                                + path
                                + "' does not allow null. Declare it nullable (@Nullable). "
                                + "@ZLinkJsonOptional only allows omission."),
                error);
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    @ZLinkPacket(PACKET)
    public record Notice(String text, long sequence, @ZLinkJsonOptional Detail detail) {}

    public record Detail(@JsonProperty("wire_text") String text) {}

    public record Barrier() {}

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;
        final CompletableFuture<Void> handlerStarted = new CompletableFuture<>();
        final CompletableFuture<Void> handlerTerminal = new CompletableFuture<>();

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(NoticeHandler.class);
            context.handlers().addPacket(BarrierHandler.class);
        }
    }

    public static final class NoticeHandler implements ZLinkSpotPacketHandler<Instance, Notice> {
        @Override
        public CompletionStage<Void> handle(Instance spot, Notice notice) {
            if (!notice.text().equals("fail")) return CompletableFuture.completedFuture(null);
            spot.handlerStarted.complete(null);
            return spot.handlerTerminal;
        }
    }

    public static final class BarrierHandler
            implements ZLinkSpotRequestHandler<Instance, Barrier, Barrier> {
        @Override
        public CompletionStage<Barrier> handle(Instance spot, Barrier barrier) {
            return CompletableFuture.completedFuture(barrier);
        }
    }

    private static final class Fixture extends Handler implements AutoCloseable {
        private final Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        private final List<String> records = new CopyOnWriteArrayList<>();
        private final String spotId = UUID.randomUUID().toString();
        private final ZLinkFrameworkRuntime runtime;
        private final ZLinkInstanceSpotActivation activation;
        private final Instance instance;

        Fixture() throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.DETAILED);
            options.addLocationStore(new ZLinkInMemoryLocationStore());
            options.addRelocationStore(new InMemoryRelocationStore());
            options.addRouteMesh(MESH)
                    .listen("inproc://" + spotId)
                    .objects()
                    .server()
                    .addInstanceSpotFactory(
                            SPOT_TYPE, Instance.class, factory -> factory.disableRelocation());
            runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
            logger.addHandler(this);
            try {
                await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
                await(
                        runtime.route()
                                .requestToSpot(spotId, new Barrier())
                                .instanceSpot(SPOT_TYPE)
                                .inMesh(MESH)
                                .timeout(Duration.ofSeconds(WAIT_SECONDS))
                                .submit(Barrier.class));
                activation =
                        ZLinkFrameworkRuntimeTestAccess.spots(runtime)
                                .instanceSpotActivations
                                .get(spotId);
                instance = (Instance) activation.spotSurface;
            } catch (Exception | AssertionError failure) {
                close();
                throw failure;
            }
        }

        RoutedPacket receive(String json) {
            var flow = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
            String correlation = ZLinkChannelEnvelope.newCorrelationId();
            var header =
                    new ZLinkChannelEnvelope.Header(
                            ZLinkChannelEnvelope.KIND_COMMAND,
                            "",
                            PACKET,
                            ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                            correlation,
                            null,
                            null,
                            null,
                            null,
                            null,
                            Map.of(),
                            flow.flowId(),
                            flow.origin());
            var received =
                    new ZLinkBackendReceived(
                            Optional.of(RoutingId.from("source-node")),
                            Optional.of(spotId),
                            Optional.empty(),
                            List.of(ZLinkChannelEnvelope.encodeHeader(header), Message.from(json)),
                            null);
            return new RoutedPacket(flow, correlation, activation.admitRoute(received));
        }

        List<String> records(RoutedPacket packet) {
            return records.stream()
                    .filter(line -> line.contains("flow=" + packet.flow().flowId()))
                    .toList();
        }

        String error(RoutedPacket packet, String reason) throws Exception {
            assertNotNull(await(packet.terminal().handle((ignored, failure) -> failure)));
            ZLinkTelemetryTestFence.awaitIdle();
            var errors =
                    records(packet).stream()
                            .filter(line -> line.contains("event_id=zlink.dispatch_error"))
                            .toList();
            assertEquals(1, errors.size(), records.toString());
            String error = errors.getFirst();
            assertTrue(error.contains("outcome=failed"), error);
            assertTrue(error.contains("reason=" + reason), error);
            assertTrue(error.contains("action=drop"), error);
            assertTrue(error.contains("corr=" + packet.correlation()), error);
            assertTrue(error.contains("origin=application"), error);
            return error;
        }

        @Override
        public void publish(LogRecord record) {
            if (record.getMessage().contains(" packet=" + PACKET)) records.add(record.getMessage());
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            try {
                if (instance != null) instance.handlerTerminal.complete(null);
                runtime.close();
            } finally {
                ZLinkTelemetryTestFence.awaitIdle();
                logger.removeHandler(this);
            }
        }
    }

    private record RoutedPacket(
            ZLinkFlowContext.State flow, String correlation, CompletionStage<Void> terminal) {}
}
