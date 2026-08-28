package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.StreamSocket;
import java.io.InputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.time.Duration;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfMultiStreamRegressionTest {
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
}
