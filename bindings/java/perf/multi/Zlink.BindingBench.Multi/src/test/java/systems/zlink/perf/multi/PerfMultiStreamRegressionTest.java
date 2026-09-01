package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.StreamSocket;
import java.io.InputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfMultiStreamRegressionTest {
    private static final Path RUNNER = Path.of("..", "run_benchmarks.sh");
    private static final Path SERVER_SOURCE = Path.of("src", "main", "java",
        "systems", "zlink", "perf", "multi", "PerfMultiStream.java");

    @Test
    void runnerResolvesNonTcpCapOnceForServerAndClient() throws Exception {
        String runner = Files.readString(RUNNER, StandardCharsets.UTF_8);
        String resolver = functionBody(runner,
            "effective_clients_for_transport", "pick_endpoint");
        String streamCase = functionBody(runner,
            "run_stream_case", "run_socket_case");

        assertTrue(resolver.contains("${pattern}\" == \"MULTI_STREAM"));
        assertTrue(resolver.contains("${transport}\" != \"tcp"));
        assertTrue(resolver.contains("${STREAM_NON_TCP_CLIENTS_MAX}"));
        assertFalse(streamCase.contains("STREAM_NON_TCP_CLIENTS_MAX"));
        assertTrue(streamCase.contains(
            "build_multi_role_cmd stream_server_cmd"));
        assertTrue(streamCase.contains("--ccu \"${stream_clients}\""));
        assertTrue(runner.contains("--clients \"${pattern_clients}\""));

        int resolution = runner.lastIndexOf(
            "pattern_clients=\"$(effective_clients_for_transport");
        int invocation = runner.indexOf(
            "run_stream_case \"${case_connect_concurrency}\"", resolution);
        assertTrue(resolution >= 0);
        assertTrue(invocation > resolution,
            "the effective client count must be resolved before the STREAM case starts");

        String server = Files.readString(SERVER_SOURCE,
            StandardCharsets.UTF_8);
        assertTrue(server.contains(
            "MonitorEventType.CONNECTION_READY, config.clients()"));
    }

    @Test
    void framedPacketEchoReturnsOriginalWireBytes() throws Exception {
        AtomicBoolean stopRequested = new AtomicBoolean(false);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Object stopSignal = new Object();

        byte[] header = "hello-header".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] body = new byte[64];
        java.util.Arrays.fill(body, (byte) 'x');
        byte[] frame = buildFrame(header, body);

        int port = freePort();
        String endpoint = "tcp://127.0.0.1:" + port;

        try (Context ctx = Zlink.createContext();
             StreamSocket server = ctx.createStreamSocket()) {
            server.options().notify(true);
            Duration timeout = Duration.ofSeconds(5);
            server.options().sendTimeout(timeout);
            server.bind(endpoint);
            try (PerfMultiStream.StreamReplyDispatcher dispatcher =
                     new PerfMultiStream.StreamReplyDispatcher(server,
                         timeout, timeout, stopRequested, stopSignal,
                         failure)) {
                server.onPacket(dispatcher::onPacket);

                try (Socket client = new Socket("127.0.0.1", port)) {
                    client.setSoTimeout((int) timeout.toMillis());
                    client.getOutputStream().write(frame);
                    client.getOutputStream().flush();
                    byte[] echoedFrame = readExact(
                        client.getInputStream(), frame.length);
                    assertArrayEquals(frame, echoedFrame);
                }

                dispatcher.stopAndDrain();
            }

            assertNull(failure.get(),
                "dispatcher raised: " + failure.get());
        }
    }

    private static byte[] buildFrame(byte[] header, byte[] body) {
        byte[] frame = new byte[6 + header.length + body.length];
        frame[0] = (byte) ((header.length >>> 8) & 0xFF);
        frame[1] = (byte) (header.length & 0xFF);
        frame[2] = (byte) ((body.length >>> 24) & 0xFF);
        frame[3] = (byte) ((body.length >>> 16) & 0xFF);
        frame[4] = (byte) ((body.length >>> 8) & 0xFF);
        frame[5] = (byte) (body.length & 0xFF);
        System.arraycopy(header, 0, frame, 6, header.length);
        System.arraycopy(body, 0, frame, 6 + header.length, body.length);
        return frame;
    }

    private static byte[] readExact(InputStream input, int length) throws Exception {
        byte[] data = input.readNBytes(length);
        assertTrue(data.length == length,
            "expected " + length + " bytes but received " + data.length);
        return data;
    }

    private static int freePort() throws Exception {
        try (ServerSocket socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return socket.getLocalPort();
        }
    }

    private static String functionBody(String source, String name,
                                       String nextName) {
        int start = source.indexOf(name + "() {");
        int end = source.indexOf(nextName + "() {", start);
        assertTrue(start >= 0, "function not found: " + name);
        assertTrue(end > start, "function boundary not found: " + nextName);
        return source.substring(start, end);
    }
}
