package MyGame.lobby.infrastructure

import MyGame.*
import MyGame.lobby.application.*
import io.netty.bootstrap.ServerBootstrap
import io.netty.buffer.ByteBufInputStream
import io.netty.buffer.Unpooled
import io.netty.channel.*
import io.netty.channel.group.DefaultChannelGroup
import io.netty.channel.nio.NioEventLoopGroup
import io.netty.channel.socket.ChannelInputShutdownEvent
import io.netty.channel.socket.SocketChannel
import io.netty.channel.socket.nio.NioServerSocketChannel
import io.netty.handler.codec.http.*
import java.io.IOException
import java.net.InetSocketAddress
import java.net.URI
import java.util.concurrent.*
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.spots.*

class ZLinkRooms(
    private val spots: ZLinkSpotManager,
    private val route: ZLinkRouteClient,
    private val config: NodeConfig,
) :
    RoomProvisioner {
    override fun prepare(roomId: String, members: List<String>) =
        spots.getOrCreate(roomId, Names.ROOM).inMesh(Names.MESH).submit().thenCompose { created ->
            check(created.state() != ZLinkSpotCreateState.REJECTED) { "Room creation rejected" }
            check(created.spot().spotId() == roomId) { "Created Room identity differs from the match." }
            route
                .requestToSpot(roomId, PrepareRoom(roomId, members))
                .inMesh(Names.MESH)
                .submit(RoomPrepared::class.java)
                .thenApply { prepared ->
                    check(prepared.matchId == roomId) { "Prepared Room identity differs from the match." }
                    MatchResponse(roomId, requireNotNull(config.sessionEndpoint))
                }
        }
}

class HttpLobby(config: NodeConfig, private val matches: Matchmaker) : AutoCloseable, HttpEndpoint {
    private val io = NioEventLoopGroup()
    private val channels = DefaultChannelGroup(io.next())
    private val server: Channel

    init {
        val uri = URI.create(requireNotNull(config.httpEndpoint))
        try {
            server =
                ServerBootstrap()
                    .group(io)
                    .channel(NioServerSocketChannel::class.java)
                    .childOption(ChannelOption.ALLOW_HALF_CLOSURE, true)
                    .childHandler(
                        object : ChannelInitializer<SocketChannel>() {
                            override fun initChannel(channel: SocketChannel) {
                                channels.add(channel)
                                channel
                                    .pipeline()
                                    .addLast(
                                        HttpServerCodec(),
                                        HttpObjectAggregator(Int.MAX_VALUE),
                                        RequestHandler(),
                                    )
                            }
                        }
                    )
                    .bind(uri.host, uri.port)
                    .syncUninterruptibly()
                    .channel()
        } catch (error: RuntimeException) {
            io.shutdownGracefully(0, 0, TimeUnit.SECONDS).syncUninterruptibly()
            throw IOException("HTTP listener failed", error)
        }
    }

    override fun endpoint(): String {
        val address = server.localAddress() as InetSocketAddress
        return "http://${address.hostString}:${address.port}"
    }

    private inner class RequestHandler : SimpleChannelInboundHandler<FullHttpRequest>() {
        private lateinit var request: MatchRequest
        private var pending: CompletableFuture<MatchResponse>? = null

        override fun channelRead0(ctx: ChannelHandlerContext, message: FullHttpRequest) {
            if (pending != null) return
            if (
                message.method().name() != HttpProtocol.POST ||
                    QueryStringDecoder(message.uri()).path() != LobbyNames.MATCH_PATH
            ) {
                reply(ctx, HttpProtocol.NOT_FOUND, null)
                return
            }
            val result: CompletableFuture<MatchResponse>
            try {
                request =
                    Json.mapper.readValue(
                        ByteBufInputStream(message.content()) as java.io.InputStream,
                        MatchRequest::class.java,
                    )
                result = matches.match(request).toCompletableFuture()
                pending = result
            } catch (error: IOException) {
                reply(ctx, HttpProtocol.BAD_REQUEST, null)
                return
            } catch (error: IllegalArgumentException) {
                reply(ctx, HttpProtocol.BAD_REQUEST, null)
                return
            }
            ctx.channel().closeFuture().addListener { matches.cancelWaiting(request, result) }
            result.whenComplete { assignment, error ->
                ctx.executor().execute {
                    if (!ctx.channel().isActive) return@execute
                    if (error != null) {
                        if (error !is CancellationException) {
                            error.printStackTrace(System.err)
                            reply(ctx, HttpProtocol.UNAVAILABLE, null)
                        }
                    } else {
                        try {
                            reply(ctx, HttpProtocol.OK, Json.mapper.writeValueAsBytes(assignment))
                        } catch (failure: IOException) {
                            failure.printStackTrace(System.err)
                            reply(ctx, HttpProtocol.UNAVAILABLE, null)
                        }
                    }
                }
            }
        }

        override fun userEventTriggered(ctx: ChannelHandlerContext, event: Any) {
            if (event is ChannelInputShutdownEvent) {
                // Remove an unpaired waiter before closing the peer's half-closed connection.
                val result = pending
                if (result == null || matches.cancelWaiting(request, result)) ctx.close()
            } else super.userEventTriggered(ctx, event)
        }

        override fun exceptionCaught(ctx: ChannelHandlerContext, error: Throwable) {
            error.printStackTrace(System.err)
            ctx.close()
        }
    }

    private fun reply(ctx: ChannelHandlerContext, status: Int, body: ByteArray?) {
        val response =
            DefaultFullHttpResponse(
                HttpVersion.HTTP_1_1,
                HttpResponseStatus.valueOf(status),
                if (body == null) Unpooled.EMPTY_BUFFER else Unpooled.wrappedBuffer(body),
            )
        if (body != null) response.headers().set(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON)
        HttpUtil.setContentLength(response, response.content().readableBytes().toLong())
        HttpUtil.setKeepAlive(response, false)
        ctx.writeAndFlush(response).addListener(ChannelFutureListener.CLOSE)
    }

    override fun close() {
        server.close().syncUninterruptibly()
        matches.close()
        channels.close().awaitUninterruptibly()
        io.shutdownGracefully(0, 0, TimeUnit.SECONDS).syncUninterruptibly()
    }
}
