package systems.zlink.stream.connector;
import java.net.InetSocketAddress;
import java.util.concurrent.TimeUnit;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.SimpleChannelInboundHandler;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.HttpServerCodec;
import io.netty.handler.codec.http.websocketx.BinaryWebSocketFrame;
import io.netty.handler.codec.http.websocketx.WebSocketServerProtocolHandler;
import io.netty.handler.ssl.SslContext;
import io.netty.handler.ssl.SslContextBuilder;
import java.io.Closeable;
import java.net.URI;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;

final class SecureWebSocketStreamConnectorTestServer implements Closeable {
    private final EventLoopGroup boss = new NioEventLoopGroup(1);
    private final EventLoopGroup worker = new NioEventLoopGroup(1);
    private final Queue<CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame>> waiters =
        new ArrayDeque<>();
    private final Queue<TcpStreamConnectorTestServer.ReceivedFrame> frames = new ArrayDeque<>();
    private final Channel serverChannel;
    private volatile Channel clientChannel;

    SecureWebSocketStreamConnectorTestServer() throws Exception {
        SslContext sslContext = SslContextBuilder
            .forServer(TlsTestCertificates.certificate(), TlsTestCertificates.privateKey())
            .build();
        serverChannel = new ServerBootstrap()
            .group(boss, worker)
            .channel(NioServerSocketChannel.class)
            .childHandler(new ChannelInitializer<SocketChannel>() {
                @Override
                protected void initChannel(SocketChannel channel) {
                    clientChannel = channel;
                    channel.pipeline().addLast(sslContext.newHandler(channel.alloc()));
                    channel.pipeline().addLast(new HttpServerCodec());
                    channel.pipeline().addLast(new HttpObjectAggregator(8192));
                    channel.pipeline().addLast(
                        new WebSocketServerProtocolHandler("/stream", null, true));
                    channel.pipeline().addLast(new BinaryFrameHandler(
                        SecureWebSocketStreamConnectorTestServer.this));
                }
            })
            .bind("127.0.0.1", 0)
            .sync()
            .channel();
    }

    URI endpoint() {
        return URI.create("wss://127.0.0.1:" + port() + "/stream");
    }

    ZLinkStreamConnectorOptions options(ZLinkStreamDispatchMode dispatchMode) {
        return new ZLinkStreamConnectorOptions(
            endpoint(),
            dispatchMode,
            Duration.ofSeconds(1),
            1,
            Duration.ofSeconds(1),
            64 * 1024,
            false,
            Duration.ofMillis(25),
            Duration.ofMillis(500),
            true,
            Duration.ofMillis(10),
            Duration.ofMillis(250),
            2.0,
            true);
    }

    CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> readFrameAsync() {
        synchronized (this) {
            if (!frames.isEmpty()) {
                return CompletableFuture.completedFuture(frames.remove());
            }
            CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> waiter =
                new CompletableFuture<>();
            waiters.add(waiter);
            return waiter;
        }
    }

    CompletableFuture<Void> sendAsync(
        ZLinkStreamWireProtocol.Header header,
        byte[] payload) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        byte[] encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
        byte[] frame = ZLinkStreamWireProtocol.encodeFrame(encodedHeader, payload, 64 * 1024);
        Channel channel = awaitClientChannel();
        channel.writeAndFlush(new BinaryWebSocketFrame(Unpooled.wrappedBuffer(frame)))
            .addListener(write -> {
                if (write.isSuccess()) {
                    result.complete(null);
                } else {
                    result.completeExceptionally(write.cause());
                }
            });
        return result;
    }

    @Override
    public void close() {
        if (clientChannel != null) {
            clientChannel.close();
        }
        serverChannel.close();
        boss.shutdownGracefully();
        worker.shutdownGracefully();
    }

    private int port() {
        return ((InetSocketAddress) serverChannel.localAddress()).getPort();
    }

    private Channel awaitClientChannel() {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            Channel channel = clientChannel;
            if (channel != null && channel.isActive()) {
                return channel;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("wss client did not connect within 5s");
    }

    private void enqueue(TcpStreamConnectorTestServer.ReceivedFrame frame) {
        CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> waiter;
        synchronized (this) {
            waiter = waiters.poll();
            if (waiter == null) {
                frames.add(frame);
                return;
            }
        }
        waiter.complete(frame);
    }

    private static final class BinaryFrameHandler
        extends SimpleChannelInboundHandler<BinaryWebSocketFrame> {
        private final SecureWebSocketStreamConnectorTestServer server;

        private BinaryFrameHandler(SecureWebSocketStreamConnectorTestServer server) {
            this.server = server;
        }

        @Override
        protected void channelRead0(ChannelHandlerContext context, BinaryWebSocketFrame frame) {
            byte[] message = new byte[frame.content().readableBytes()];
            frame.content().readBytes(message);
            ZLinkStreamWireProtocol.Frame decoded =
                ZLinkStreamWireProtocol.decodeFrame(message);
            server.enqueue(new TcpStreamConnectorTestServer.ReceivedFrame(
                ZLinkStreamWireProtocol.decodeHeader(decoded.header()),
                decoded.payload()));
        }

        @Override
        public void exceptionCaught(ChannelHandlerContext context, Throwable cause) {
            context.close();
        }
    }
}
