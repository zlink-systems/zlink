package systems.zlink.stream.connector;

import java.io.BufferedReader;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class WebSocketStreamConnectorTestServer implements Closeable {
    private static final String MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    private final ServerSocket server;
    private final ExecutorService executor = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "zlink-ws-test-server");
        thread.setDaemon(true);
        return thread;
    });
    private volatile Socket current;

    WebSocketStreamConnectorTestServer() throws IOException {
        server = new ServerSocket(0);
        executor.execute(this::acceptLoop);
    }

    URI endpoint() {
        return URI.create("ws://127.0.0.1:" + server.getLocalPort() + "/stream");
    }

    CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> readFrameAsync() {
        return CompletableFuture.supplyAsync(() -> {
            try {
                byte[] message = readBinaryMessage(socket());
                ZLinkStreamWireProtocol.Frame frame =
                    ZLinkStreamWireProtocol.decodeFrame(message);
                return new TcpStreamConnectorTestServer.ReceivedFrame(
                    ZLinkStreamWireProtocol.decodeHeader(frame.header()),
                    frame.payload());
            } catch (Exception ex) {
                throw new RuntimeException(ex);
            }
        }, executor);
    }

    CompletableFuture<Void> sendAsync(
        ZLinkStreamWireProtocol.Header header,
        byte[] payload) {
        return CompletableFuture.runAsync(() -> {
            try {
                byte[] encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
                byte[] frame = ZLinkStreamWireProtocol.encodeFrame(
                    encodedHeader,
                    payload,
                    64 * 1024);
                writeBinaryMessage(socket().getOutputStream(), frame);
            } catch (Exception ex) {
                throw new RuntimeException(ex);
            }
        }, executor);
    }

    CompletableFuture<Void> sendRawAsync(byte[] payload) {
        return CompletableFuture.runAsync(() -> {
            try {
                writeBinaryMessage(socket().getOutputStream(), payload);
            } catch (Exception ex) {
                throw new RuntimeException(ex);
            }
        }, executor);
    }

    ZLinkStreamConnectorOptions options(ZLinkStreamDispatchMode dispatchMode) {
        return new ZLinkStreamConnectorOptions(
            endpoint(),
            dispatchMode,
            java.time.Duration.ofSeconds(1),
            1,
            java.time.Duration.ofSeconds(1),
            64 * 1024,
            false,
            java.time.Duration.ofMillis(25),
            java.time.Duration.ofMillis(500),
            true,
            java.time.Duration.ofMillis(10),
            java.time.Duration.ofMillis(250),
            2.0);
    }

    @Override
    public void close() throws IOException {
        try {
            if (current != null) {
                current.close();
            }
            server.close();
        } finally {
            executor.shutdownNow();
        }
    }

    private void acceptLoop() {
        while (!server.isClosed()) {
            try {
                Socket socket = server.accept();
                completeHandshake(socket);
                current = socket;
            } catch (IOException ex) {
                if (!server.isClosed()) {
                    throw new RuntimeException(ex);
                }
            }
        }
    }

    private Socket socket() {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            Socket socket = current;
            if (socket != null && !socket.isClosed()) {
                return socket;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("websocket client did not connect within 5s");
    }

    private static void completeHandshake(Socket socket) throws IOException {
        BufferedReader reader = new BufferedReader(new InputStreamReader(
            socket.getInputStream(),
            StandardCharsets.US_ASCII));
        String key = null;
        String line;
        while ((line = reader.readLine()) != null && !line.isEmpty()) {
            String lower = line.toLowerCase(Locale.ROOT);
            if (lower.startsWith("sec-websocket-key:")) {
                key = line.substring(line.indexOf(':') + 1).trim();
            }
        }
        if (key == null) {
            throw new IOException("missing websocket key");
        }
        String accept = acceptKey(key);
        String response = "HTTP/1.1 101 Switching Protocols\r\n"
            + "Upgrade: websocket\r\n"
            + "Connection: Upgrade\r\n"
            + "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        socket.getOutputStream().write(response.getBytes(StandardCharsets.US_ASCII));
        socket.getOutputStream().flush();
    }

    private static String acceptKey(String key) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-1");
            byte[] hash = digest.digest((key + MAGIC).getBytes(StandardCharsets.US_ASCII));
            return Base64.getEncoder().encodeToString(hash);
        } catch (Exception ex) {
            throw new IllegalStateException(ex);
        }
    }

    private static byte[] readBinaryMessage(Socket socket) throws IOException {
        DataInputStream input = new DataInputStream(socket.getInputStream());
        int first = input.readUnsignedByte();
        int opcode = first & 0x0f;
        if (opcode != 2) {
            throw new IOException("expected binary websocket message");
        }
        int second = input.readUnsignedByte();
        boolean masked = (second & 0x80) != 0;
        int length = second & 0x7f;
        if (length == 126) {
            length = input.readUnsignedShort();
        } else if (length == 127) {
            long longLength = input.readLong();
            if (longLength > Integer.MAX_VALUE) {
                throw new IOException("websocket message too large");
            }
            length = (int) longLength;
        }
        byte[] mask = masked ? input.readNBytes(4) : new byte[0];
        byte[] payload = input.readNBytes(length);
        if (masked) {
            for (int i = 0; i < payload.length; i++) {
                payload[i] = (byte) (payload[i] ^ mask[i % 4]);
            }
        }
        return payload;
    }

    private static void writeBinaryMessage(OutputStream output, byte[] payload) throws IOException {
        output.write(0x82);
        if (payload.length < 126) {
            output.write(payload.length);
        } else if (payload.length <= 0xffff) {
            output.write(126);
            output.write(ByteBuffer.allocate(2).putShort((short) payload.length).array());
        } else {
            output.write(127);
            output.write(ByteBuffer.allocate(8).putLong(payload.length).array());
        }
        output.write(payload);
        output.flush();
    }
}
