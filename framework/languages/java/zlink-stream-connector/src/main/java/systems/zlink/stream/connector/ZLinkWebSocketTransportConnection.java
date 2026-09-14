package systems.zlink.stream.connector;
import java.io.EOFException;
import java.io.ByteArrayOutputStream;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.WebSocket;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkWebSocketTransportConnection
    implements ZLinkStreamTransportConnection, WebSocket.Listener {
    private final CompletableFuture<ZLinkWebSocketTransportConnection> opened =
        new CompletableFuture<>();
    private final Queue<ZLinkStreamWireProtocol.Frame> frames = new ArrayDeque<>();
    private final Queue<CompletableFuture<ZLinkStreamWireProtocol.Frame>> waiters =
        new ArrayDeque<>();
    private ByteArrayOutputStream message;
    private volatile WebSocket webSocket;
    private volatile Throwable failure;
    private final int maxReceivePayloadSize;

    private ZLinkWebSocketTransportConnection(int maxReceivePayloadSize) {
        this.maxReceivePayloadSize = maxReceivePayloadSize;
    }

    static CompletionStage<ZLinkWebSocketTransportConnection> connectStage(
        HttpClient client,
        URI endpoint,
        int maxReceivePayloadSize) {
        ZLinkWebSocketTransportConnection connection =
            new ZLinkWebSocketTransportConnection(maxReceivePayloadSize);
        client.newWebSocketBuilder()
            .buildAsync(endpoint, connection)
            .whenComplete((socket, ex) -> {
                if (ex != null) {
                    connection.fail(ex);
                }
            });
        return connection.opened;
    }

    @Override
    public void onOpen(WebSocket socket) {
        webSocket = socket;
        opened.complete(this);
        WebSocket.Listener.super.onOpen(socket);
    }

    @Override
    public CompletionStage<?> onBinary(WebSocket socket, ByteBuffer data, boolean last) {
        ZLinkStreamWireProtocol.Frame frame = null;
        try {
            synchronized (this) {
                if (failure != null) {
                    return CompletableFuture.completedFuture(null);
                }
                int received = message == null ? 0 : message.size();
                if ((long) received + data.remaining()
                        > ZLinkStreamWireProtocol.maxFrameLength(maxReceivePayloadSize)) {
                    throw new IllegalArgumentException(
                        "websocket frame exceeds max receive payload size");
                }
                if (message == null) {
                    message = new ByteArrayOutputStream();
                }
                byte[] chunk = new byte[data.remaining()];
                data.get(chunk);
                message.writeBytes(chunk);
                if (last) {
                    byte[] complete = message.toByteArray();
                    message = null;
                    frame = ZLinkStreamWireProtocol.decodeFrame(complete, maxReceivePayloadSize);
                }
            }
            if (frame != null) {
                enqueue(frame);
            }
        } catch (RuntimeException ex) {
            fail(ex);
        }
        socket.request(1);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<?> onClose(WebSocket socket, int statusCode, String reason) {
        fail(new EOFException("websocket closed"));
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public void onError(WebSocket socket, Throwable error) {
        fail(error);
    }

    @Override
    public CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync() {
        synchronized (this) {
            if (!frames.isEmpty()) {
                return CompletableFuture.completedFuture(frames.remove());
            }
            if (failure != null) {
                return CompletableFuture.failedFuture(failure);
            }
            CompletableFuture<ZLinkStreamWireProtocol.Frame> waiter = new CompletableFuture<>();
            waiters.add(waiter);
            return waiter;
        }
    }

    @Override
    public CompletionStage<Void> writeAsync(byte[] frame) {
        WebSocket current = webSocket;
        if (current == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("websocket is not connected"));
        }
        return current.sendBinary(ByteBuffer.wrap(frame), true)
            .thenApply(ignored -> null);
    }

    @Override
    public boolean isOpen() {
        WebSocket current = webSocket;
        return current != null && !current.isInputClosed() && !current.isOutputClosed();
    }

    @Override
    public void close() {
        WebSocket current = webSocket;
        if (current != null) {
            current.abort();
        }
        fail(new EOFException("websocket closed"));
    }

    private void enqueue(ZLinkStreamWireProtocol.Frame frame) {
        CompletableFuture<ZLinkStreamWireProtocol.Frame> waiter;
        synchronized (this) {
            if (failure != null) {
                return;
            }
            waiter = waiters.poll();
            if (waiter == null) {
                frames.add(frame);
                return;
            }
        }
        waiter.complete(frame);
    }

    private void fail(Throwable ex) {
        Queue<CompletableFuture<ZLinkStreamWireProtocol.Frame>> pending;
        synchronized (this) {
            if (failure == null) {
                failure = ex;
            }
            message = null;
            opened.completeExceptionally(ex);
            pending = new ArrayDeque<>(waiters);
            waiters.clear();
        }
        for (CompletableFuture<ZLinkStreamWireProtocol.Frame> waiter : pending) {
            waiter.completeExceptionally(ex);
        }
    }
}
