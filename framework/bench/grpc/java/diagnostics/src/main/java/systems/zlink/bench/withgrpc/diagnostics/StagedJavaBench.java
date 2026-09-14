/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.diagnostics;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.security.MessageDigest;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.bench.withgrpc.client.BenchDrivers;
import systems.zlink.bench.withgrpc.client.BenchOperation;
import systems.zlink.bench.withgrpc.client.BenchOptions;
import systems.zlink.bench.withgrpc.client.BenchResultWriter;
import systems.zlink.bench.withgrpc.client.RawStack;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.Args;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.bench.withgrpc.shared.BenchStatsServer;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.configuration.ZLinkBenchCodecBridge;

/**
 * Test-only raw-binding A/B fixture.  {@link FixedDiagnosticStage#VALUE} is generated
 * while compiling, so the stage is never selected by an environment variable or a hot
 * path branch.  Both stages retain the protobuf BenchPayload contract; {@code codec}
 * additionally enters the real Framework protobuf extension/serializer boundary.
 */
public final class StagedJavaBench {
    public static final int REQUEST_BYTES = 64;
    public static final int RESPONSE_BYTES = 4096;
    public static final int SEND_BYTES = 4096;
    public static final String REQUEST_SERVER_ID = "bench-staged-request-server";
    public static final String SEND_SERVER_ID = "bench-staged-send-server";

    private StagedJavaBench() {
    }

    public static void main(String[] args) throws Exception {
        switch (Args.value(args, "--role", "source")) {
            case "source" -> runJavaSource(args);
            case "target" -> runTarget(args);
            case "fidelity" -> runFidelity(args);
            default -> throw new IllegalArgumentException("--role must be source or target");
        }
    }

    static void runJavaSource(String[] args) throws Exception {
        BenchOptions options = new BenchOptions(args);
        if (options.payloadSizes.get(0) != RESPONSE_BYTES) {
            throw new IllegalArgumentException("staged logical payload must be 4096 bytes");
        }
        BenchDrivers drivers = new BenchDrivers(options);
        AtomicBoolean ready = new AtomicBoolean();
        StageClient[] client = new StageClient[1];
        BenchHttpApplication.Controller[] holder = new BenchHttpApplication.Controller[1];
        BenchHttpApplication.Controller controller = new BenchHttpApplication.Controller(
            ready::get, drivers::counters, trigger -> {
                BenchOperation operation = client[0].operation(options.scenario);
                if ("warmup".equals(trigger.phase())) {
                    drivers.runWarmup(trigger, operation);
                    return;
                }
                BenchResultWriter.write(options, holder[0].lastTrigger(),
                    drivers.runActive(trigger, operation), "java",
                    "one Java raw-binding submitter; protobuf retained; stage="
                        + FixedDiagnosticStage.VALUE,
                    Map.of("diagnosticStage", FixedDiagnosticStage.VALUE,
                        "requestPayloadBytes", REQUEST_BYTES,
                        "responsePayloadBytes", RESPONSE_BYTES,
                        "sendPayloadBytes", SEND_BYTES,
                        "runtimeScope", "raw binding plus optional protobuf codec boundary"));
            });
        holder[0] = controller;
        BenchHttpApplication http = BenchHttpApplication.start(options.triggerUrl, options.statsUrl,
            controller);
        try {
            client[0] = StageClient.open(options);
            drivers.waitForRouteReady(client[0].operation(options.scenario), RESPONSE_BYTES);
            ready.set(true);
        } catch (Throwable error) {
            http.close();
            throw error;
        }
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            ready.set(false);
            http.close();
            if (client[0] != null) {
                client[0].close();
            }
        }, "bench-staged-java-source-shutdown"));
        Thread.currentThread().join();
    }

    private static void runTarget(String[] args) throws Exception {
        String requestEndpoint = Args.value(args, "--endpoint", "tcp://127.0.0.1:5284");
        String sendEndpoint = Args.value(args, "--send-endpoint", "tcp://127.0.0.1:5285");
        String metricsUrl = Args.value(args, "--metrics-url", "http://127.0.0.1:5286");
        Context context = Zlink.createContext();
        RouterSocket request = context.createRouterSocket();
        RouterSocket send = context.createRouterSocket();
        request.setRoutingId(RoutingId.from(REQUEST_SERVER_ID.getBytes(StandardCharsets.US_ASCII)));
        send.setRoutingId(RoutingId.from(SEND_SERVER_ID.getBytes(StandardCharsets.US_ASCII)));
        request.options().mandatory(true);
        send.options().mandatory(true);
        request.bind(requestEndpoint);
        send.bind(sendEndpoint);
        BenchServerMetrics metrics = new BenchServerMetrics();
        BenchStatsServer.start(metricsUrl, metrics,
            "{\"implementation\":\"zlink-staged-jvm\",\"stage\":\""
                + FixedDiagnosticStage.VALUE + "\"}");
        Thread.ofPlatform().daemon(true).name("bench-staged-request")
            .start(() -> serveRequests(request, metrics));
        Thread.ofPlatform().daemon(true).name("bench-staged-send")
            .start(() -> serveSends(send, metrics));
        Thread.currentThread().join();
    }

    /** Finite, non-throughput check of the compiled stage's protobuf and size contract. */
    private static void runFidelity(String[] args) throws Exception {
        int runId = 0x5a11;
        long sequence = 17;
        BenchPayload request = fidelityPayload(REQUEST_BYTES, runId, sequence);
        BenchPayload reply = fidelityPayload(RESPONSE_BYTES, runId, sequence);
        BenchPayload send = fidelityPayload(SEND_BYTES, runId, sequence);
        byte[] requestFrame;
        byte[] replyFrame;
        byte[] sendFrame;
        try (Message encoded = encode(request)) { requestFrame = bytes(encoded.dataBuffer()); }
        try (Message encoded = encode(reply)) { replyFrame = bytes(encoded.dataBuffer()); }
        try (Message encoded = encode(send)) { sendFrame = bytes(encoded.dataBuffer()); }
        try (Message encoded = Message.from(requestFrame)) {
            assertHeader(decode(encoded), runId, REQUEST_BYTES, sequence);
        }
        try (Message encoded = Message.from(replyFrame)) {
            assertHeader(decode(encoded), runId, RESPONSE_BYTES, sequence);
        }
        try (Message encoded = Message.from(sendFrame)) {
            assertHeader(decode(encoded), runId, SEND_BYTES, sequence);
        }
        String json = "{\"stage\":\"" + FixedDiagnosticStage.VALUE
            + "\",\"requestBodyBytes\":" + REQUEST_BYTES
            + ",\"responseBodyBytes\":" + RESPONSE_BYTES
            + ",\"sendBodyBytes\":" + SEND_BYTES
            + ",\"requestFrameSha256\":\"" + sha256(requestFrame)
            + "\",\"responseFrameSha256\":\"" + sha256(replyFrame)
            + "\",\"sendFrameSha256\":\"" + sha256(sendFrame) + "\"}\n";
        String output = Args.value(args, "--fidelity-output", "");
        if (!output.isBlank()) Files.writeString(Path.of(output), json, StandardCharsets.UTF_8);
        System.out.print(json);
    }

    private static void assertHeader(BenchPayload payload, int runId, int bodyBytes, long sequence) {
        BenchMetricHeader.Decoded header = BenchMetricHeader.decode(payload.getBody().asReadOnlyByteBuffer());
        if (!BenchMetricHeader.isExpected(header, runId, BenchMetricHeader.PHASE_ACTIVE,
            bodyBytes, sequence)) {
            throw new IllegalStateException("staged fidelity header mismatch");
        }
    }

    private static BenchPayload fidelityPayload(int bodyBytes, int runId, long sequence) {
        byte[] body = BenchMetricHeader.createPayload(bodyBytes, runId,
            BenchMetricHeader.PHASE_ACTIVE, sequence);
        // Timestamp measures latency in a live run.  It is deliberately zeroed only
        // here so independent compile outputs have identical protobuf input bytes.
        ByteBuffer.wrap(body).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(21, 0L);
        return BenchPayload.newBuilder().setBody(com.google.protobuf.ByteString.copyFrom(body)).build();
    }

    private static String sha256(byte[] value) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(value);
        StringBuilder text = new StringBuilder(digest.length * 2);
        for (byte element : digest) text.append(String.format("%02x", element));
        return text.toString();
    }

    private static void serveRequests(RouterSocket socket, BenchServerMetrics metrics) {
        try (Received received = new Received()) {
            while (true) {
                if (!socket.recv(received, RecvFlags.NONE)) continue;
                try {
                    BenchPayload request = decode(lastPart(received));
                    ByteBuffer body = request.getBody().asReadOnlyByteBuffer();
                    BenchMetricHeader.Decoded header = BenchMetricHeader.decode(body);
                    if (header == null) throw new IllegalArgumentException("missing benchmark header");
                    metrics.record(body);
                    try (Message envelope = Message.from(RawWire.RESPONSE_ENVELOPE);
                         Message reply = encode(response(header))) {
                        received.reply().message(envelope).message(reply).submit();
                    }
                } catch (Throwable error) {
                    metrics.recordError();
                    System.err.println("staged request failed: " + error);
                }
            }
        }
    }

    private static void serveSends(RouterSocket socket, BenchServerMetrics metrics) {
        try (Received received = new Received()) {
            while (true) {
                if (!socket.recv(received, RecvFlags.NONE)) continue;
                try {
                    metrics.record(decode(lastPart(received)).getBody().asReadOnlyByteBuffer());
                } catch (Throwable error) {
                    metrics.recordError();
                    System.err.println("staged send failed: " + error);
                }
            }
        }
    }

    private static Message lastPart(Received received) {
        List<Message> parts = received.parts();
        if (parts.isEmpty()) throw new IllegalArgumentException("missing payload part");
        return parts.get(parts.size() - 1);
    }

    private static final ZLinkBenchCodecBridge.Session TARGET_CODEC =
        "codec".equals(FixedDiagnosticStage.VALUE) ? ZLinkBenchCodecBridge.open() : null;

    public static Message encode(BenchPayload value) {
        if ("core".equals(FixedDiagnosticStage.VALUE)) {
            return Message.from(value.toByteArray());
        }
        return TARGET_CODEC.encode(value);
    }

    public static BenchPayload decode(Message frame) {
        if ("core".equals(FixedDiagnosticStage.VALUE)) {
            return parse(bytes(frame.dataBuffer()));
        }
        return TARGET_CODEC.decode(frame);
    }

    private static BenchPayload response(BenchMetricHeader.Decoded request) {
        return BenchPayload.newBuilder().setBody(com.google.protobuf.ByteString.copyFrom(
            BenchMetricHeader.createPayload(RESPONSE_BYTES, request.runId(), request.phase(),
                request.sequence()))).build();
    }

    private static BenchPayload parse(byte[] bytes) {
        try {
            return BenchPayload.parseFrom(bytes);
        } catch (com.google.protobuf.InvalidProtocolBufferException error) {
            throw new IllegalArgumentException("invalid BenchPayload", error);
        }
    }

    private static byte[] bytes(ByteBuffer source) {
        ByteBuffer copy = source.duplicate();
        byte[] value = new byte[copy.remaining()];
        copy.get(value);
        return value;
    }

    public static RawStack.PayloadCodec sourceCodec(int runId) {
        if ("core".equals(FixedDiagnosticStage.VALUE)) return RawStack.PayloadCodec.RAW_WIRE;
        ZLinkBenchCodecBridge.Session session = ZLinkBenchCodecBridge.open();
        return new RawStack.PayloadCodec() {
            @Override public Message encode(int size, int ignored, byte phase, long sequence) {
                return session.encode(BenchPayload.newBuilder().setBody(
                    com.google.protobuf.ByteString.copyFrom(
                        BenchMetricHeader.createPayload(size, runId, phase, sequence))).build());
            }
            @Override public ByteBuffer decode(Message frame) {
                return session.decode(frame).getBody().asReadOnlyByteBuffer();
            }
        };
    }

    private static final class StageClient implements AutoCloseable {
        private final Context context;
        private final RawStack request;
        private final RawStack send;
        private final RawStack.PayloadCodec codec;

        private StageClient(Context context, RawStack request, RawStack send,
            RawStack.PayloadCodec codec) {
            this.context = context;
            this.request = request;
            this.send = send;
            this.codec = codec;
        }

        static StageClient open(BenchOptions options) {
            Context context = Zlink.createContext();
            RawStack request = RawStack.create(context, options,
                "bench-staged-java-request-" + ProcessHandle.current().pid(),
                REQUEST_SERVER_ID, options.targetEndpoint);
            RawStack send = RawStack.create(context, options,
                "bench-staged-java-send-" + ProcessHandle.current().pid(),
                SEND_SERVER_ID, options.targetCommandEndpoint);
            return new StageClient(context, request, send, sourceCodec(options.runId));
        }

        BenchOperation operation(String scenario) {
            return "send-saturation".equals(scenario)
                ? send.send(codec)
                : request.request(REQUEST_BYTES, RESPONSE_BYTES, codec);
        }
        @Override public void close() { request.close(); send.close(); context.close(); }
    }
}
