package MyGame.lobby.infrastructure;

import MyGame.*;
import MyGame.lobby.application.Matchmaker;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.Unpooled;
import io.netty.channel.*;
import io.netty.channel.group.DefaultChannelGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.ChannelInputShutdownEvent;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.http.*;

import java.io.IOException;
import java.net.*;
import java.util.concurrent.*;

public final class HttpLobby implements AutoCloseable, Hosting.HttpEndpoint {
    private final EventLoopGroup io = new NioEventLoopGroup();
    private final DefaultChannelGroup channels = new DefaultChannelGroup(io.next());
    private final Channel server;
    private final Matchmaker matches;

    public HttpLobby(NodeConfig config, Matchmaker matches) throws IOException {
        this.matches = matches;
        var uri = URI.create(config.httpEndpoint());
        try {
            server =
                    new ServerBootstrap()
                            .group(io)
                            .channel(NioServerSocketChannel.class)
                            .childOption(ChannelOption.ALLOW_HALF_CLOSURE, true)
                            .childHandler(
                                    new ChannelInitializer<SocketChannel>() {
                                        protected void initChannel(SocketChannel channel) {
                                            channels.add(channel);
                                            channel.pipeline()
                                                    .addLast(
                                                            new HttpServerCodec(),
                                                            new HttpObjectAggregator(
                                                                    Integer.MAX_VALUE),
                                                            new RequestHandler());
                                        }
                                    })
                            .bind(uri.getHost(), uri.getPort())
                            .syncUninterruptibly()
                            .channel();
        } catch (RuntimeException error) {
            io.shutdownGracefully(0, 0, TimeUnit.SECONDS).syncUninterruptibly();
            throw new IOException("HTTP listener failed", error);
        }
    }

    public String endpoint() {
        var address = (InetSocketAddress) server.localAddress();
        return "http://" + address.getHostString() + ":" + address.getPort();
    }

    private final class RequestHandler extends SimpleChannelInboundHandler<FullHttpRequest> {
        private MatchRequest request;
        private CompletableFuture<MatchResponse> pending;

        protected void channelRead0(ChannelHandlerContext ctx, FullHttpRequest message) {
            if (pending != null) return;
            if (!message.method().name().equals(HttpProtocol.POST)
                    || !new QueryStringDecoder(message.uri()).path().equals(Names.MATCH_PATH)) {
                reply(ctx, HttpProtocol.NOT_FOUND, null);
                return;
            }
            try {
                request =
                        Json.MAPPER.readValue(
                                (java.io.InputStream)
                                        new io.netty.buffer.ByteBufInputStream(message.content()),
                                MatchRequest.class);
                pending = matches.match(request).toCompletableFuture();
            } catch (IOException | IllegalArgumentException error) {
                reply(ctx, HttpProtocol.BAD_REQUEST, null);
                return;
            }
            ctx.channel()
                    .closeFuture()
                    .addListener(ignored -> matches.cancelWaiting(request, pending));
            pending.whenComplete(
                    (assignment, error) ->
                            ctx.executor()
                                    .execute(
                                            () -> {
                                                if (!ctx.channel().isActive()) return;
                                                if (error != null) {
                                                    if (!(error instanceof CancellationException)) {
                                                        error.printStackTrace(System.err);
                                                        reply(ctx, HttpProtocol.UNAVAILABLE, null);
                                                    }
                                                    return;
                                                }
                                                try {
                                                    reply(
                                                            ctx,
                                                            HttpProtocol.OK,
                                                            Json.MAPPER.writeValueAsBytes(
                                                                    assignment));
                                                } catch (IOException failure) {
                                                    failure.printStackTrace(System.err);
                                                    reply(ctx, HttpProtocol.UNAVAILABLE, null);
                                                }
                                            }));
        }

        public void userEventTriggered(ChannelHandlerContext ctx, Object event) throws Exception {
            if (event instanceof ChannelInputShutdownEvent) {
                // Remove an unpaired waiter before closing the peer's half-closed connection.
                if (pending == null || matches.cancelWaiting(request, pending)) ctx.close();
            } else super.userEventTriggered(ctx, event);
        }

        public void exceptionCaught(ChannelHandlerContext ctx, Throwable error) {
            error.printStackTrace(System.err);
            ctx.close();
        }
    }

    private static void reply(ChannelHandlerContext ctx, int status, byte[] body) {
        var response =
                new DefaultFullHttpResponse(
                        HttpVersion.HTTP_1_1,
                        HttpResponseStatus.valueOf(status),
                        body == null ? Unpooled.EMPTY_BUFFER : Unpooled.wrappedBuffer(body));
        if (body != null) response.headers().set(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON);
        HttpUtil.setContentLength(response, response.content().readableBytes());
        HttpUtil.setKeepAlive(response, false);
        ctx.writeAndFlush(response).addListener(ChannelFutureListener.CLOSE);
    }

    public void close() {
        server.close().syncUninterruptibly();
        matches.close();
        channels.close().awaitUninterruptibly();
        io.shutdownGracefully(0, 0, TimeUnit.SECONDS).syncUninterruptibly();
    }
}
