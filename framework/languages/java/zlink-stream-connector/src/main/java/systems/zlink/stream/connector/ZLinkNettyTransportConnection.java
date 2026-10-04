package systems.zlink.stream.connector;

import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.ByteBufAllocator;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFuture;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.codec.ByteToMessageDecoder;
import io.netty.handler.codec.DecoderException;
import io.netty.handler.ssl.SslContext;
import io.netty.handler.ssl.SslContextBuilder;
import io.netty.handler.ssl.SslHandler;
import io.netty.handler.ssl.util.InsecureTrustManagerFactory;

import java.io.EOFException;
import java.net.URI;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ThreadFactory;

import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;

final class ZLinkNettyTransportConnection implements ZLinkStreamTransportConnection {
    private static final int LIFECYCLE_OWNS_CONNECT_DEADLINE = 0;
    private static final EventLoopGroup EVENT_LOOP =
            new NioEventLoopGroup(0, new DaemonThreadFactory());

    private final Queue<ZLinkStreamWireProtocol.Frame> frames = new ArrayDeque<>();
    private final Queue<CompletableFuture<ZLinkStreamWireProtocol.Frame>> waiters =
            new ArrayDeque<>();
    private volatile Channel channel;
    private volatile Throwable failure;

    private ZLinkNettyTransportConnection() {}

    static CompletionStage<ZLinkNettyTransportConnection> connectStage(
            URI endpoint,
            ZLinkStreamTransport transport,
            int maxReceivePayloadSize,
            boolean skipServerCertificateValidation) {
        CompletableFuture<ZLinkNettyTransportConnection> result = new CompletableFuture<>();
        ZLinkNettyTransportConnection connection = new ZLinkNettyTransportConnection();
        SslContext sslContext;
        try {
            if (transport == ZLinkStreamTransport.TLS) {
                SslContextBuilder builder = SslContextBuilder.forClient();
                if (skipServerCertificateValidation) {
                    builder.trustManager(InsecureTrustManagerFactory.INSTANCE);
                }
                sslContext = builder.build();
            } else {
                sslContext = null;
            }
        } catch (SSLException ex) {
            return CompletableFuture.failedFuture(ex);
        }

        int port = DefaultZLinkStreamConnector.resolvePort(endpoint);
        Bootstrap bootstrap =
                new Bootstrap()
                        .group(EVENT_LOOP)
                        .channel(NioSocketChannel.class)
                        .option(ChannelOption.SO_KEEPALIVE, true)
                        .option(
                                ChannelOption.CONNECT_TIMEOUT_MILLIS,
                                LIFECYCLE_OWNS_CONNECT_DEADLINE)
                        .handler(
                                new ChannelInitializer<SocketChannel>() {
                                    @Override
                                    protected void initChannel(SocketChannel channel) {
                                        if (sslContext != null) {
                                            channel.pipeline()
                                                    .addLast(
                                                            createSslHandler(
                                                                    sslContext,
                                                                    channel.alloc(),
                                                                    endpoint.getHost(),
                                                                    port,
                                                                    skipServerCertificateValidation));
                                        }
                                        channel.pipeline()
                                                .addLast(new FrameDecoder(maxReceivePayloadSize));
                                        channel.pipeline().addLast(new InboundHandler(connection));
                                    }
                                });

        DefaultZLinkStreamConnector.trace(() -> "connector connect-start endpoint=" + endpoint);
        ChannelFuture connecting = bootstrap.connect(endpoint.getHost(), port);
        connection.channel = connecting.channel();
        result.whenComplete(
                (opened, failure) -> {
                    if (failure != null) connection.close();
                });
        connecting.addListener(
                connect -> {
                    if (!connect.isSuccess()) {
                        DefaultZLinkStreamConnector.trace(
                                () ->
                                        "connector connect-failed endpoint="
                                                + endpoint
                                                + " error="
                                                + connect.cause());
                        connection.fail(connect.cause());
                        result.completeExceptionally(connect.cause());
                        return;
                    }
                    DefaultZLinkStreamConnector.trace(
                            () -> "connector connect-complete endpoint=" + endpoint);
                    Channel connected = ((ChannelFuture) connect).channel();
                    SslHandler sslHandler = connected.pipeline().get(SslHandler.class);
                    if (sslHandler == null) {
                        if (!result.complete(connection)) connection.close();
                        return;
                    }
                    sslHandler
                            .handshakeFuture()
                            .addListener(
                                    handshake -> {
                                        if (handshake.isSuccess()) {
                                            if (!result.complete(connection)) connection.close();
                                        } else {
                                            connection.fail(handshake.cause());
                                            connected.close();
                                            result.completeExceptionally(handshake.cause());
                                        }
                                    });
                });
        return result;
    }

    static SslHandler createSslHandler(
            SslContext sslContext,
            ByteBufAllocator allocator,
            String host,
            int port,
            boolean skipServerCertificateValidation) {
        SslHandler handler = sslContext.newHandler(allocator, host, port);
        handler.setHandshakeTimeoutMillis(LIFECYCLE_OWNS_CONNECT_DEADLINE);
        if (!skipServerCertificateValidation) {
            SSLParameters parameters = handler.engine().getSSLParameters();
            parameters.setEndpointIdentificationAlgorithm("HTTPS");
            handler.engine().setSSLParameters(parameters);
        }
        return handler;
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
        Channel current = channel;
        if (current == null || !current.isActive()) {
            return CompletableFuture.failedFuture(
                    ZLinkStreamException.disconnected("stream transport is not connected"));
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        current.writeAndFlush(Unpooled.wrappedBuffer(frame))
                .addListener(
                        write -> {
                            if (write.isSuccess()) {
                                result.complete(null);
                            } else {
                                result.completeExceptionally(write.cause());
                            }
                        });
        return result;
    }

    @Override
    public boolean isOpen() {
        Channel current = channel;
        return current != null && current.isActive();
    }

    /**
     * Spec 32 7: closing the transport does not wait for the peer. {@code Channel.close()} would
     * pass through the {@link SslHandler}, which queues close_notify behind the frames not yet
     * flushed and keeps the socket open until the peer reads them or its flush timeout ends. The
     * close starts at the SslHandler's own context instead, which hands it to the socket below that
     * handler. A pending channel without that handler closes directly.
     */
    @Override
    public void close() {
        Channel current = channel;
        ChannelHandlerContext tls =
                current == null ? null : current.pipeline().context(SslHandler.class);
        if (tls != null) {
            tls.close();
        } else if (current != null) {
            current.close();
        }
        fail(new EOFException("stream transport closed"));
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
        for (CompletableFuture<ZLinkStreamWireProtocol.Frame> waiter : pending) {
            waiter.completeExceptionally(ex);
        }
    }

    private static final class FrameDecoder extends ByteToMessageDecoder {
        private final int maxReceivePayloadSize;

        private FrameDecoder(int maxReceivePayloadSize) {
            this.maxReceivePayloadSize = maxReceivePayloadSize;
        }

        @Override
        protected void decode(ChannelHandlerContext context, ByteBuf input, List<Object> output) {
            if (input.readableBytes() < ZLinkStreamWireProtocol.FRAME_PREFIX_BYTES) {
                return;
            }
            input.markReaderIndex();
            int headerLength = input.readUnsignedShort();
            int payloadLength = input.readInt();
            int bodyLength =
                    ZLinkStreamWireProtocol.checkedBodyLength(
                            headerLength, payloadLength, maxReceivePayloadSize);
            if (input.readableBytes() < bodyLength) {
                input.resetReaderIndex();
                return;
            }
            byte[] header = new byte[headerLength];
            byte[] payload = new byte[payloadLength];
            input.readBytes(header);
            input.readBytes(payload);
            output.add(new ZLinkStreamWireProtocol.Frame(header, payload));
        }
    }

    private static final class InboundHandler extends ChannelInboundHandlerAdapter {
        private final ZLinkNettyTransportConnection connection;

        private InboundHandler(ZLinkNettyTransportConnection connection) {
            this.connection = connection;
        }

        @Override
        public void channelRead(ChannelHandlerContext context, Object message) {
            connection.enqueue((ZLinkStreamWireProtocol.Frame) message);
        }

        @Override
        public void channelInactive(ChannelHandlerContext context) {
            connection.fail(new EOFException("stream transport closed"));
        }

        @Override
        public void exceptionCaught(ChannelHandlerContext context, Throwable cause) {
            //  Netty wraps what the frame decoder threw; the read fails with that failure.
            connection.fail(
                    cause instanceof DecoderException && cause.getCause() != null
                            ? cause.getCause()
                            : cause);
            context.close();
        }
    }

    private static final class DaemonThreadFactory implements ThreadFactory {
        @Override
        public Thread newThread(Runnable runnable) {
            Thread thread = new Thread(runnable, "zlink-stream-connector-netty");
            thread.setDaemon(true);
            return thread;
        }
    }
}
