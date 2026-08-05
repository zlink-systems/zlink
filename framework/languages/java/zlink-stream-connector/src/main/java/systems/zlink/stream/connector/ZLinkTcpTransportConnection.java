package systems.zlink.stream.connector;

import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.AsynchronousSocketChannel;
import java.nio.channels.CompletionHandler;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkTcpTransportConnection implements ZLinkStreamTransportConnection {
    private final AsynchronousSocketChannel channel;
    private final int maxReceivePayloadSize;

    ZLinkTcpTransportConnection(
        AsynchronousSocketChannel channel,
        int maxReceivePayloadSize) {
        this.channel = channel;
        this.maxReceivePayloadSize = maxReceivePayloadSize;
    }

    AsynchronousSocketChannel channel() {
        return channel;
    }

    @Override
    public CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync() {
        ByteBuffer prefix = ByteBuffer.allocate(6);
        return readFully(channel, prefix).thenCompose(ignored -> {
            prefix.flip();
            int headerLength = Short.toUnsignedInt(prefix.getShort());
            int payloadLength = prefix.getInt();
            int bodyLength;
            try {
                bodyLength = ZLinkStreamWireProtocol.checkedBodyLength(
                    headerLength,
                    payloadLength,
                    maxReceivePayloadSize);
            } catch (IllegalArgumentException ex) {
                return CompletableFuture.failedFuture(ex);
            }
            ByteBuffer body = ByteBuffer.allocate(bodyLength);
            return readFully(channel, body).thenApply(ignoredBody -> {
                body.flip();
                byte[] header = new byte[headerLength];
                byte[] payload = new byte[payloadLength];
                body.get(header);
                body.get(payload);
                return new ZLinkStreamWireProtocol.Frame(header, payload);
            });
        });
    }

    @Override
    public CompletionStage<Void> writeAsync(byte[] frame) {
        return writeFully(channel, ByteBuffer.wrap(frame));
    }

    @Override
    public boolean isOpen() {
        return channel.isOpen();
    }

    @Override
    public void close() {
        try {
            channel.close();
        } catch (IOException ignored) {
        }
    }

    static CompletableFuture<Void> readFully(
        AsynchronousSocketChannel channel,
        ByteBuffer buffer) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        readFully(channel, buffer, result);
        return result;
    }

    private static void readFully(
        AsynchronousSocketChannel channel,
        ByteBuffer buffer,
        CompletableFuture<Void> result) {
        if (!buffer.hasRemaining()) {
            result.complete(null);
            return;
        }
        channel.read(buffer, null, new CompletionHandler<Integer, Void>() {
            @Override
            public void completed(Integer count, Void attachment) {
                if (count == null || count < 0) {
                    result.completeExceptionally(new EOFException("stream closed"));
                    return;
                }
                readFully(channel, buffer, result);
            }

            @Override
            public void failed(Throwable exc, Void attachment) {
                result.completeExceptionally(exc);
            }
        });
    }

    private static CompletableFuture<Void> writeFully(
        AsynchronousSocketChannel channel,
        ByteBuffer buffer) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        writeFully(channel, buffer, result);
        return result;
    }

    private static void writeFully(
        AsynchronousSocketChannel channel,
        ByteBuffer buffer,
        CompletableFuture<Void> result) {
        if (!buffer.hasRemaining()) {
            result.complete(null);
            return;
        }
        channel.write(buffer, null, new CompletionHandler<Integer, Void>() {
            @Override
            public void completed(Integer count, Void attachment) {
                if (count == null || count < 0) {
                    result.completeExceptionally(new EOFException("stream closed"));
                    return;
                }
                writeFully(channel, buffer, result);
            }

            @Override
            public void failed(Throwable exc, Void attachment) {
                result.completeExceptionally(exc);
            }
        });
    }
}
