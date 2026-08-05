package systems.zlink.stream.connector;

import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBufAllocator;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.codec.ByteToMessageDecoder;
import io.netty.handler.ssl.SslContext;
import io.netty.handler.ssl.SslContextBuilder;
import io.netty.handler.ssl.SslHandler;
import io.netty.handler.ssl.util.InsecureTrustManagerFactory;
import java.net.URI;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ThreadFactory;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLException;

final class ZLinkTlsTransportConnection implements ZLinkStreamTransportConnection {
    private static final EventLoopGroup EVENT_LOOP =
        new NioEventLoopGroup(0, new DaemonThreadFactory());

    private final Queue<ZLinkStreamWireProtocol.Frame> frames = new ArrayDeque<>();
    private final Queue<CompletableFuture<ZLinkStreamWireProtocol.Frame>> waiters =
        new ArrayDeque<>();
    private volatile Channel channel;
    private volatile Throwable failure;

    private ZLinkTlsTransportConnection() {
    }

    static CompletionStage<ZLinkTlsTransportConnection> connectStage(
        URI endpoint,
        Duration connectTimeout,
        int maxReceivePayloadSize,
        boolean skipServerCertificateValidation) {
        CompletableFuture<ZLinkTlsTransportConnection> result = new CompletableFuture<>();
        ZLinkTlsTransportConnection connection = new ZLinkTlsTransportConnection();
        SslContext sslContext;
        try {
            SslContextBuilder builder = SslContextBuilder.forClient();
            if (skipServerCertificateValidation) {
                builder.trustManager(InsecureTrustManagerFactory.INSTANCE);
            }
            sslContext = builder.build();
        } catch (SSLException ex) {
            return CompletableFuture.failedFuture(ex);
        }

        int port = DefaultZLinkStreamConnector.resolvePort(endpoint);
        Bootstrap bootstrap = new Bootstrap()
            .group(EVENT_LOOP)
            .channel(NioSocketChannel.class)
            .option(ChannelOption.SO_KEEPALIVE, true)
            .option(ChannelOption.CONNECT_TIMEOUT_MILLIS, Math.toIntExact(connectTimeout.toMillis()))
            .handler(new ChannelInitializer<SocketChannel>() {
                @Override
                protected void initChannel(SocketChannel channel) {
                    channel.pipeline().addLast(createSslHandler(
                        sslContext,
                        channel.alloc(),
                        endpoint.getHost(),
                        port,
                        skipServerCertificateValidation));
                    channel.pipeline().addLast(new FrameDecoder(maxReceivePayloadSize));
                    channel.pipeline().addLast(new InboundHandler(connection));
                }
            });

        bootstrap.connect(endpoint.getHost(), port).addListener(connect -> {
            if (!connect.isSuccess()) {
                connection.fail(connect.cause());
                result.completeExceptionally(connect.cause());
                return;
            }
            Channel connected = ((io.netty.channel.ChannelFuture) connect).channel();
            connection.channel = connected;
            connected.pipeline()
                .get(io.netty.handler.ssl.SslHandler.class)
                .handshakeFuture()
                .addListener(handshake -> {
                    if (handshake.isSuccess()) {
                        result.complete(connection);
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
                new IllegalStateException("tls transport is not connected"));
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        current.writeAndFlush(Unpooled.wrappedBuffer(frame)).addListener(write -> {
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

    @Override
    public void close() {
        Channel current = channel;
        if (current != null) {
            current.close();
        }
        fail(new java.io.EOFException("tls transport closed"));
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
            if (input.readableBytes() < 6) {
                return;
            }
            input.markReaderIndex();
            int headerLength = input.readUnsignedShort();
            int payloadLength = input.readInt();
            int bodyLength = ZLinkStreamWireProtocol.checkedBodyLength(
                headerLength,
                payloadLength,
                maxReceivePayloadSize);
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
        private final ZLinkTlsTransportConnection connection;

        private InboundHandler(ZLinkTlsTransportConnection connection) {
            this.connection = connection;
        }

        @Override
        public void channelRead(ChannelHandlerContext context, Object message) {
            connection.enqueue((ZLinkStreamWireProtocol.Frame) message);
        }

        @Override
        public void channelInactive(ChannelHandlerContext context) {
            connection.fail(new java.io.EOFException("tls transport closed"));
        }

        @Override
        public void exceptionCaught(ChannelHandlerContext context, Throwable cause) {
            connection.fail(cause);
            context.close();
        }
    }

    private static final class DaemonThreadFactory implements ThreadFactory {
        @Override
        public Thread newThread(Runnable runnable) {
            Thread thread = new Thread(runnable, "zlink-stream-connector-tls");
            thread.setDaemon(true);
            return thread;
        }
    }
}
