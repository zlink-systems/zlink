package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ManualScheduledExecutor;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamRuntimeTestAccess;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.StandardProtocolFamily;
import java.net.URI;
import java.net.UnixDomainSocketAddress;
import java.net.http.HttpClient;
import java.net.http.WebSocket;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.SocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyStore;
import java.security.cert.CertificateFactory;
import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BooleanSupplier;
import java.util.function.LongSupplier;
import java.util.stream.Stream;

import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManagerFactory;

final class StreamPermitLivenessTest {
    private static final Duration INTERVAL = Duration.ofMillis(200);
    private static final Duration HEARTBEAT = Duration.ofMillis(1000);
    private static final Duration IDLE = Duration.ofMillis(450);
    private static final Path CERTS = Path.of("../../../../bindings/java/tests/certs");
    private static final String PING = ZLinkStreamHeader.HEARTBEAT_PING.name();
    private static final String PONG = ZLinkStreamHeader.HEARTBEAT_PONG.name();
    private static final String CLOSED = Metric.STREAM_CONNECTIONS_CLOSED.metricName();

    enum Scenario {
        UNSATURATED(1500, null),
        BELOW_TIMEOUT(300, null),
        AT_TIMEOUT(1000, null),
        ABOVE_TIMEOUT(1500, null),
        SILENT_AFTER_RELEASE(1500, "heartbeat_timeout"),
        IDLE_AFTER_RELEASE(1500, "idle_timeout");

        final long holdMillis;
        final String reason;

        Scenario(long holdMillis, String reason) {
            this.holdMillis = holdMillis;
            this.reason = reason;
        }
    }

    static Stream<Arguments> cases() {
        var transports = Stream.of("tcp", "tls", "ws", "wss");
        if (Zlink.has("ipc")) {
            transports = Stream.concat(transports, Stream.of("ipc"));
        }
        return transports.flatMap(
                transport ->
                        Arrays.stream(Scenario.values())
                                .map(scenario -> Arguments.of(transport, scenario)));
    }

    @ParameterizedTest(name = "{0} {1}")
    @MethodSource("cases")
    void permitWaitSuspendsReceiveDeadlines(
            String transport, Scenario scenario, @TempDir Path tempDir) throws Exception {
        verify(transport, scenario, tempDir);
    }

    @ParameterizedTest(name = "inproc {0}")
    @EnumSource(Scenario.class)
    void inProcessPermitWaitSuspendsReceiveDeadlines(Scenario scenario) throws Exception {
        verify("inproc", scenario, null);
    }

    @Test
    void hostStreamTimesOutAfterPermitWaitAndResumedReceive() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        options.addStreamNode("gateway")
                .bind("tcp://127.0.0.1:0")
                .heartbeat(INTERVAL, HEARTBEAT)
                .registerSession(ZLinkMissingLocalActorTest.Session.class);

        var queue = options.registration().applicationJobQueue();
        var waitLimit = Duration.ofSeconds(10);
        try (var metrics = ZLinkRuntimeMetricProbe.install();
                var host =
                        ZLinkFrameworkRuntimeTestAccess.startHost(
                                options, new ZLinkJavaBackendAdapterFactory());
                var peer =
                        new Peer(
                                URI.create(
                                        host.listenerStatus(ZLinkListenerKind.STREAM, "gateway")
                                                .endpoint()),
                                null,
                                System::nanoTime)) {
            peer.openSession();
            try (var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(waitLimit.toMillis(), TimeUnit.MILLISECONDS)) {
                peer.ping();
                await(() -> queue.snapshot().capacityWaiters() == 1, waitLimit);

                // The controlled-clock cases assert exact deadlines and heartbeat phases.
                Thread.sleep(HEARTBEAT.plus(INTERVAL));
                peer.assertNoPong();
                assertFalse(peer.sessionClosing.isDone(), "session closed during permit wait");
            }

            peer.awaitPong();
            var closing = peer.sessionClosing.get(waitLimit.toMillis(), TimeUnit.MILLISECONDS);
            assertEquals(3, closing.reason());
            await(() -> closeReasons(metrics).contains("heartbeat_timeout"), waitLimit);
        }
    }

    private void verify(String transport, Scenario scenario, Path tempDir) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        String bind =
                switch (transport) {
                    case "ipc" -> "ipc://" + tempDir.resolve("stream.sock");
                    case "inproc" -> "inproc://stream-permit-liveness";
                    default -> transport + "://127.0.0.1:0";
                };
        var node =
                options.addStreamNode("gateway")
                        .bind(bind)
                        .heartbeat(INTERVAL, HEARTBEAT)
                        .registerSession(ZLinkMissingLocalActorTest.Session.class);
        if (transport.equals("tls") || transport.equals("wss")) {
            node.setTlsServer(
                    CERTS.resolve("server.crt").toString(), CERTS.resolve("server.key").toString());
        }
        boolean idle = scenario == Scenario.IDLE_AFTER_RELEASE;
        if (idle) {
            node.idleTimeout(IDLE);
        }

        var registration = options.registration();
        var backend = new ZLinkJavaBackendAdapterFactory();
        try (var metrics = ZLinkRuntimeMetricProbe.install();
                var context = new ZLinkJavaContext(Zlink.createContext());
                var handlerExecutor = Executors.newVirtualThreadPerTaskExecutor();
                var serialExecutor = registration.serialExecutor();
                var clock = new LivenessClock()) {
            options.useHandlerExecutor(handlerExecutor);
            var runtime =
                    ZLinkStreamRuntimeTestAccess.create(
                            backend, registration, context, clock::now, clock.scheduler);
            try (AutoCloseable stop = () -> runtime.closeAsync().toCompletableFuture().join()) {
                runtime.start();
                var endpoint =
                        URI.create(
                                runtime.boundListenerEndpoint(
                                        registration.streamNodes().getFirst()));
                var inproc = transport.equals("inproc") ? context.nativeContext() : null;
                try (var peer = new Peer(endpoint, inproc, clock::now)) {
                    exercise(
                            transport,
                            scenario,
                            idle,
                            registration.applicationJobQueue(),
                            metrics,
                            peer,
                            endpoint,
                            inproc,
                            clock);
                }
            }
        }
    }

    private void exercise(
            String transport,
            Scenario scenario,
            boolean idle,
            ZLinkApplicationJobQueue queue,
            ZLinkRuntimeMetricProbe metrics,
            Peer peer,
            URI endpoint,
            Context inproc,
            LivenessClock clock)
            throws Exception {
        peer.openSession();
        await(() -> clock.scheduler.hasTimerDueAt(INTERVAL.toNanos()), Duration.ofSeconds(2));
        if (scenario == Scenario.UNSATURATED) {
            long step = Duration.ofMillis(40).toNanos();
            long until = TimeUnit.MILLISECONDS.toNanos(scenario.holdMillis);
            int pongs = 0;
            for (long tick = 0; tick < until; tick += step) {
                clock.advanceTo(tick);
                peer.ping();
                peer.awaitPong();
                pongs++;
            }
            assertTrue(pongs > 1);
            assertFalse(peer.sessionClosing.isDone(), "peer closed while receiving pings");
            return;
        }

        // Give the connection its first heartbeat phase before starting the permit scenario.
        clock.advanceThroughPings(INTERVAL.toNanos(), peer);
        // Separate the original receive deadline from the exact-timeout hold.
        clock.advanceTo(clock.now() + Duration.ofMillis(100).toNanos());
        boolean silent = scenario == Scenario.SILENT_AFTER_RELEASE;
        // PACKET ingress shares a reader. A second peer wakes it without refreshing the
        // observed session's heartbeat after release.
        try (var other = silent ? new Peer(endpoint, inproc, clock::now) : null) {
            if (other != null) {
                other.openSession();
                await(
                        () -> clock.scheduler.hasTimerDueAt(clock.now() + INTERVAL.toNanos()),
                        Duration.ofSeconds(2));
            }
            long released;
            var probe = silent ? other : peer;
            var held =
                    queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            try {
                probe.ping();
                await(() -> queue.snapshot().capacityWaiters() == 1, Duration.ofSeconds(2));
                peer.clearFrames();
                long until = clock.now() + TimeUnit.MILLISECONDS.toNanos(scenario.holdMillis);
                int serverPings = clock.advanceThroughPings(until, peer);
                peer.assertNoPong();
                var reasons = closeReasons(metrics);
                System.out.printf(
                        "HOLD %s %s pings=%d closes=%s%n",
                        transport, scenario, serverPings, reasons);
                assertTrue(reasons.isEmpty(), "closed during permit wait: " + reasons);
                assertTrue(
                        serverPings >= Math.max(1, scenario.holdMillis / INTERVAL.toMillis() - 1));
            } finally {
                released = clock.now();
                held.close();
            }

            probe.awaitPong();
            if (scenario.reason == null) {
                assertTrue(closeReasons(metrics).isEmpty());
                return;
            }

            Duration timeout = idle ? IDLE : HEARTBEAT;
            long deadline = released + timeout.toNanos();
            if (silent) {
                clock.advanceThroughPings(released + timeout.toNanos() / 2, peer);
                other.ping();
                other.awaitPong();
            } else {
                // Heartbeat frames refresh receive liveness without refreshing application idle.
                clock.advanceThroughPings(released + Duration.ofMillis(150).toNanos(), peer);
                peer.ping();
                peer.awaitPong();
            }
            clock.advanceThroughPings(deadline - 1, peer);
            assertTrue(closeReasons(metrics).isEmpty(), "closed before one full timeout elapsed");
            assertTrue(
                    clock.scheduler.hasTimerDueAt(deadline), "reader did not rearm the deadline");
            clock.advanceTo(deadline);
            var closing = peer.awaitSessionClosing();
            await(() -> closeReasons(metrics).contains(scenario.reason), Duration.ofSeconds(2));
            long elapsed = closing.receivedNanos() - released;
            System.out.printf(
                    "RELEASE %s %s close_ms=%d reason=%s%n",
                    transport,
                    scenario,
                    TimeUnit.NANOSECONDS.toMillis(elapsed),
                    closeReasons(metrics));
            assertTrue(elapsed >= timeout.toNanos(), "closed before one full timeout elapsed");
            assertEquals(idle ? 2 : 3, closing.reason());
            assertTrue(closeReasons(metrics).contains(scenario.reason));
        }
    }

    private static final class LivenessClock implements AutoCloseable {
        private final AtomicLong clock = new AtomicLong();
        private final ManualScheduledExecutor scheduler = new ManualScheduledExecutor(clock::get);

        long now() {
            return clock.get();
        }

        void advanceTo(long nanos) {
            clock.set(nanos);
            scheduler.runDueCommands();
        }

        // A real frame acknowledges each phase before the test advances the clock again.
        int advanceThroughPings(long until, Peer peer) throws InterruptedException {
            long interval = INTERVAL.toNanos();
            long nextPhase = (now() / interval + 1) * interval;
            int pings = 0;
            for (long phase = nextPhase; phase <= until; phase += interval) {
                advanceTo(phase);
                peer.awaitPing();
                pings++;
            }
            advanceTo(until);
            return pings;
        }

        @Override
        public void close() {
            scheduler.shutdownNow();
        }
    }

    private static List<String> closeReasons(ZLinkRuntimeMetricProbe metrics) {
        return metrics.samples.stream()
                .filter(sample -> sample.name().equals(CLOSED))
                .map(sample -> sample.tags().get("close_reason"))
                .toList();
    }

    private static void await(BooleanSupplier condition, Duration timeout)
            throws InterruptedException {
        long until = System.nanoTime() + timeout.toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < until) {
            Thread.sleep(2);
        }
        assertTrue(condition.getAsBoolean(), "condition did not complete within " + timeout);
    }

    private static final class Peer implements AutoCloseable {
        private static final String TRANSPORT_CLOSED = "transport closed";

        private final LongSupplier clock;
        private final BlockingQueue<String> frames = new LinkedBlockingQueue<>();

        private record Closing(int reason, long receivedNanos) {}

        private final CompletableFuture<Closing> sessionClosing = new CompletableFuture<>();
        private final Socket socket;
        private final SocketChannel ipc;
        private final PairSocket pair;
        private final Thread pairReader;
        private final HttpClient http;
        private final WebSocket webSocket;

        Peer(URI endpoint, Context inproc, LongSupplier clock) throws Exception {
            this.clock = clock;
            String scheme = endpoint.getScheme();
            pair = inproc == null ? null : inproc.createPairSocket();
            if (pair != null) {
                socket = null;
                ipc = null;
                http = null;
                webSocket = null;
                pair.options().linger(Duration.ZERO);
                pair.connect(endpoint.toString());
                pairReader = Thread.startVirtualThread(this::readPair);
            } else if (scheme.equals("ws") || scheme.equals("wss")) {
                pairReader = null;
                var builder = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(3));
                if (scheme.equals("wss")) {
                    builder.sslContext(trustContext());
                }
                socket = null;
                ipc = null;
                http = builder.build();
                webSocket =
                        http.newWebSocketBuilder()
                                .buildAsync(
                                        endpoint,
                                        new WebSocket.Listener() {
                                            private final ByteArrayOutputStream frame =
                                                    new ByteArrayOutputStream();

                                            @Override
                                            public CompletionStage<?> onBinary(
                                                    WebSocket connection,
                                                    ByteBuffer data,
                                                    boolean last) {
                                                byte[] bytes = new byte[data.remaining()];
                                                data.get(bytes);
                                                frame.writeBytes(bytes);
                                                if (last) {
                                                    var decoded =
                                                            ZLinkStreamFrameCodec.tryDecode(
                                                                            frame.toByteArray())
                                                                    .orElseThrow();
                                                    record(decoded.header(), decoded.body());
                                                    frame.reset();
                                                }
                                                connection.request(1);
                                                return null;
                                            }

                                            @Override
                                            public CompletionStage<?> onClose(
                                                    WebSocket connection,
                                                    int statusCode,
                                                    String reason) {
                                                recordClose();
                                                return null;
                                            }

                                            @Override
                                            public void onError(
                                                    WebSocket connection, Throwable failure) {
                                                recordClose();
                                            }
                                        })
                                .get(5, TimeUnit.SECONDS);
            } else if (scheme.equals("ipc")) {
                pairReader = null;
                http = null;
                webSocket = null;
                socket = null;
                ipc = SocketChannel.open(StandardProtocolFamily.UNIX);
                ipc.connect(UnixDomainSocketAddress.of(endpoint.getPath()));
                Thread.startVirtualThread(() -> readSocket(Channels.newInputStream(ipc)));
            } else {
                pairReader = null;
                http = null;
                webSocket = null;
                ipc = null;
                socket =
                        scheme.equals("tls")
                                ? trustContext().getSocketFactory().createSocket()
                                : new Socket();
                socket.connect(new InetSocketAddress(endpoint.getHost(), endpoint.getPort()), 3000);
                var input = socket.getInputStream();
                Thread.startVirtualThread(() -> readSocket(input));
            }
        }

        private void readPair() {
            try {
                while (!Thread.currentThread().isInterrupted()) {
                    try (var packet = new Received()) {
                        if (pair.recv(packet, RecvFlags.DONT_WAIT)) {
                            var decoded =
                                    ZLinkStreamFrameCodec.tryDecode(
                                                    packet.parts().getFirst().toByteArray())
                                            .orElseThrow();
                            record(decoded.header(), decoded.body());
                            continue;
                        }
                    }
                    Thread.sleep(2);
                }
            } catch (InterruptedException stopped) {
                Thread.currentThread().interrupt();
            } finally {
                recordClose();
            }
        }

        private void readSocket(InputStream source) {
            try {
                var input = new DataInputStream(source);
                for (; ; ) {
                    int headerSize = input.readUnsignedShort();
                    int bodySize = input.readInt();
                    byte[] header = new byte[headerSize];
                    input.readFully(header);
                    byte[] body = new byte[bodySize];
                    input.readFully(body);
                    record(header, body);
                }
            } catch (java.io.IOException closed) {
                recordClose();
            }
        }

        private void record(byte[] header, byte[] body) {
            String name = ZLinkStreamHeaderCodec.decodeOrPlain(header).name();
            if (name.equals("session-closing")) {
                var payload = ByteBuffer.wrap(body);
                assertEquals(1, payload.get());
                int reason = Byte.toUnsignedInt(payload.get());
                assertEquals(body.length - 4, Short.toUnsignedInt(payload.getShort()));
                sessionClosing.complete(new Closing(reason, clock.getAsLong()));
            }
            frames.add(name);
        }

        void openSession() throws Exception {
            // A control pong can precede session creation. Ordinary ingress publishes the
            // session for this peer before the subsequent ping is received.
            send(
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.SEND,
                            ZLinkStreamCodec.RAW,
                            null,
                            Optional.empty(),
                            "fixture-ready",
                            Map.of()));
            ping();
            awaitPong();
        }

        void ping() throws Exception {
            send(ZLinkStreamHeader.HEARTBEAT_PING);
        }

        private void send(ZLinkStreamHeader header) throws Exception {
            byte[] frame = ZLinkStreamFrameCodec.encode(header, new byte[0]);
            if (pair != null) {
                try (var message = Message.from(frame)) {
                    pair.send().message(message).submit();
                }
            } else if (socket != null) {
                socket.getOutputStream().write(frame);
            } else if (ipc != null) {
                var bytes = ByteBuffer.wrap(frame);
                while (bytes.hasRemaining()) {
                    ipc.write(bytes);
                }
            } else {
                webSocket.sendBinary(ByteBuffer.wrap(frame), true).get(5, TimeUnit.SECONDS);
            }
        }

        void clearFrames() {
            assertNoPong();
            frames.clear();
        }

        void awaitPong() throws InterruptedException {
            long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            String frame;
            do {
                frame = frames.poll(Math.max(0, until - System.nanoTime()), TimeUnit.NANOSECONDS);
            } while (PING.equals(frame));
            assertEquals(PONG, frame, "expected reply to the peer's heartbeat ping");
        }

        private void recordClose() {
            frames.add(TRANSPORT_CLOSED);
        }

        Closing awaitSessionClosing() throws Exception {
            return sessionClosing.get(3, TimeUnit.SECONDS);
        }

        void assertNoPong() {
            assertFalse(frames.contains(PONG), "read a ping while the permit was held");
        }

        void awaitPing() throws InterruptedException {
            assertEquals(PING, frames.poll(3, TimeUnit.SECONDS), "expected server heartbeat ping");
        }

        @Override
        public void close() throws Exception {
            if (pair != null) {
                pairReader.interrupt();
                pairReader.join();
                pair.close();
            } else if (socket != null) {
                socket.close();
            } else if (ipc != null) {
                ipc.close();
            } else {
                webSocket.abort();
                http.close();
            }
        }

        private static SSLContext trustContext() throws Exception {
            var trust = KeyStore.getInstance(KeyStore.getDefaultType());
            trust.load(null);
            try (var ca = Files.newInputStream(CERTS.resolve("ca.crt"))) {
                trust.setCertificateEntry(
                        "fixture", CertificateFactory.getInstance("X.509").generateCertificate(ca));
            }
            var factory =
                    TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            factory.init(trust);
            var context = SSLContext.getInstance("TLS");
            context.init(null, factory.getTrustManagers(), null);
            return context;
        }
    }
}
