package systems.zlink.stream.connector;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
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
    private volatile WebSocket webSocket;
    private volatile Throwable failure;
    private final int maxReceivePayloadSize;
    //  The parts of the message being received. Only the listener thread
    //  touches it: the JDK delivers one onBinary call at a time.
    private final ByteArrayOutputStream message = new ByteArrayOutputStream();

    private ZLinkWebSocketTransportConnection(int maxReceivePayloadSize) {
        this.maxReceivePayloadSize = maxReceivePayloadSize;
    }

    static CompletionStage<ZLinkWebSocketTransportConnection> connectStage(
            HttpClient client, URI endpoint, int maxReceivePayloadSize) {
        ZLinkWebSocketTransportConnection connection =
                new ZLinkWebSocketTransportConnection(maxReceivePayloadSize);
        client.newWebSocketBuilder()
                .buildAsync(endpoint, connection)
                .whenComplete(
                        (socket, ex) -> {
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
    /**
     * Collects the parts the JDK hands over into one message and decodes it once the last part
     * arrives. A message that grows past the receive limit while it is collected is {@code
     * FrameTooLarge} (spec 32 9); its remaining parts are not kept.
     */
    public CompletionStage<?> onBinary(WebSocket socket, ByteBuffer data, boolean last) {
        if (failure == null) {
            if ((long) message.size() + data.remaining()
                    > ZLinkStreamWireProtocol.maxFrameLength(maxReceivePayloadSize)) {
                message.reset();
                fail(
                        ZLinkStreamException.of(
                                ZLinkStreamErrorCode.FRAME_TOO_LARGE,
                                "websocket message exceeds max receive payload size"));
            } else {
                byte[] part = new byte[data.remaining()];
                data.get(part);
                message.writeBytes(part);
                if (last) {
                    byte[] frame = message.toByteArray();
                    message.reset();
                    try {
                        enqueue(ZLinkStreamWireProtocol.decodeFrame(frame, maxReceivePayloadSize));
                    } catch (RuntimeException ex) {
                        fail(ex);
                    }
                }
            }
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
                    ZLinkStreamException.disconnected("websocket is not connected"));
        }
        return current.sendBinary(ByteBuffer.wrap(frame), true).thenApply(ignored -> null);
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
            pending = new ArrayDeque<>(waiters);
            waiters.clear();
        }
        //  Every future is completed outside the monitor, as the TLS
        //  transport does. Completing the connect future inside it would run
        //  the lifecycle continuation and the application connect() code
        //  while this monitor is held, and onBinary and readFrameAsync need
        //  that same monitor to take a frame in.
        opened.completeExceptionally(ex);
        for (CompletableFuture<ZLinkStreamWireProtocol.Frame> waiter : pending) {
            waiter.completeExceptionally(ex);
        }
    }
}
