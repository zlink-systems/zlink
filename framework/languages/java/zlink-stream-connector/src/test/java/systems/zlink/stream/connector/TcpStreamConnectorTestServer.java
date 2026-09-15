package systems.zlink.stream.connector;

import java.io.Closeable;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

final class TcpStreamConnectorTestServer implements Closeable {
    private final ServerSocket server;
    private final ExecutorService executor = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "zlink-stream-test-server");
        thread.setDaemon(true);
        return thread;
    });
    private final LinkedBlockingQueue<Socket> accepted = new LinkedBlockingQueue<>();
    private volatile Socket current;

    TcpStreamConnectorTestServer() throws IOException {
        this(0);
    }

    TcpStreamConnectorTestServer(int port) throws IOException {
        server = new ServerSocket(port);
        executor.execute(() -> {
            while (!server.isClosed()) {
                try {
                    accepted.add(server.accept());
                } catch (IOException ex) {
                    if (!server.isClosed()) {
                        throw new RuntimeException(ex);
                    }
                }
            }
        });
    }

    URI endpoint() {
        return URI.create("tcp://127.0.0.1:" + server.getLocalPort());
    }

    CompletableFuture<ReceivedFrame> readFrameAsync() {
        return CompletableFuture.supplyAsync(() -> {
            try {
                Socket socket = socket();
                DataInputStream input = new DataInputStream(socket.getInputStream());
                int headerLength = input.readUnsignedShort();
                int payloadLength = input.readInt();
                byte[] header = input.readNBytes(headerLength);
                byte[] payload = input.readNBytes(payloadLength);
                return new ReceivedFrame(
                    ZLinkStreamWireProtocol.decodeHeader(header),
                    payload);
            } catch (IOException ex) {
                throw new RuntimeException(ex);
            }
        }, executor);
    }

    CompletableFuture<Void> sendAsync(
        ZLinkStreamWireProtocol.Header header,
        byte[] payload) {
        byte[] encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
        return sendRawAsync(encodedHeader, payload);
    }

    CompletableFuture<Void> sendRawAsync(
        byte[] header,
        byte[] payload) {
        return CompletableFuture.runAsync(() -> {
            try {
                Socket socket = socket();
                byte[] frame = ZLinkStreamWireProtocol.encodeFrame(
                    header,
                    payload,
                    64 * 1024);
                DataOutputStream output = new DataOutputStream(socket.getOutputStream());
                output.write(frame);
                output.flush();
            } catch (IOException ex) {
                throw new RuntimeException(ex);
            }
        }, executor);
    }

    CompletableFuture<Void> sendBytesAsync(byte[] bytes) {
        return CompletableFuture.runAsync(() -> {
            try {
                Socket socket = socket();
                DataOutputStream output = new DataOutputStream(socket.getOutputStream());
                output.write(bytes);
                output.flush();
            } catch (IOException ex) {
                throw new RuntimeException(ex);
            }
        }, executor);
    }

    ZLinkStreamConnectorOptions options(ZLinkStreamDispatchMode dispatchMode) {
        return new ZLinkStreamConnectorOptions(
            endpoint(),
            dispatchMode,
            Duration.ofSeconds(1),
            1);
    }

    ZLinkStreamConnectorOptions options(
        ZLinkStreamDispatchMode dispatchMode,
        Duration requestTimeout,
        int maxReconnectAttempts,
        boolean heartbeatEnabled,
        Duration heartbeatInterval,
        Duration heartbeatTimeout,
        Duration reconnectInitialDelay) {
        return new ZLinkStreamConnectorOptions(
            endpoint(),
            dispatchMode,
            requestTimeout,
            maxReconnectAttempts,
            Duration.ofSeconds(1),
            64 * 1024,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            true,
            reconnectInitialDelay,
            Duration.ofMillis(250),
            2.0);
    }

    void closeCurrentSocket() throws IOException {
        Socket socket = current;
        current = null;
        if (socket != null) {
            socket.close();
        }
    }

    boolean hasAdditionalConnection(Duration window) {
        socket();
        try {
            Socket additional = accepted.poll(window.toMillis(), TimeUnit.MILLISECONDS);
            if (additional == null) {
                return false;
            }
            accepted.add(additional);
            return true;
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new RuntimeException(ex);
        }
    }

    static ZLinkStreamWireProtocol.Header responseTo(
        ReceivedFrame request,
        String packetName,
        Map<String, String> metadata) {
        return new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_RESPONSE,
            ZLinkStreamWireProtocol.CODEC_RAW,
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                | (metadata.isEmpty() ? 0 : ZLinkStreamWireProtocol.FLAG_HAS_METADATA),
            request.header().requestSeq(),
            packetName,
            metadata,
            null);
    }

    static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    static void awaitCondition(BooleanSupplier condition) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (condition.getAsBoolean()) {
                return;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("condition was not met within 5s");
    }

    @Override
    public void close() throws IOException {
        try {
            closeCurrentSocket();
            for (Socket socket : accepted) {
                try {
                    socket.close();
                } catch (IOException ignored) {
                }
            }
            server.close();
        } finally {
            executor.shutdownNow();
        }
    }

    private Socket socket() {
        try {
            if (current != null && !current.isClosed()) {
                return current;
            }
            Socket socket = accepted.poll(5, TimeUnit.SECONDS);
            if (socket == null) {
                throw new AssertionError("client did not connect within 5s");
            }
            current = socket;
            return socket;
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new RuntimeException(ex);
        }
    }

    record ReceivedFrame(ZLinkStreamWireProtocol.Header header, byte[] payload) {
    }

    interface BooleanSupplier {
        boolean getAsBoolean();
    }
}
