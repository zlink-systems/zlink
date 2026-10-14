package MyGame.lobby.infrastructure

import MyGame.*
import MyGame.lobby.application.*
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.io.IOException
import java.net.InetSocketAddress
import java.net.URI
import java.util.concurrent.CompletionException
import java.util.concurrent.Executors
import systems.zlink.framework.spots.*

class ZLinkRooms(private val spots: ZLinkSpotManager, private val config: NodeConfig) :
    RoomProvisioner {
    override fun prepare(roomId: String) =
        spots.getOrCreate(roomId, Names.ROOM).inMesh(Names.MESH).submit().thenApply { created ->
            check(created.state() != ZLinkSpotCreateState.REJECTED) { "Room creation rejected" }
            MatchResponse(created.spot().spotId(), requireNotNull(config.sessionEndpoint))
        }
}

class HttpLobby(config: NodeConfig, private val matches: Matchmaker) : AutoCloseable, HttpEndpoint {
    private val executor = Executors.newVirtualThreadPerTaskExecutor()
    private val server: HttpServer

    init {
        val uri = URI.create(requireNotNull(config.httpEndpoint))
        server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        server.executor = executor
        server.createContext(Names.MATCH_PATH, ::handle)
        server.start()
    }

    override fun endpoint() = "http://${server.address.hostString}:${server.address.port}"

    private fun handle(exchange: HttpExchange) {
        exchange.use {
            if (it.requestMethod != HttpProtocol.POST || it.requestURI.path != Names.MATCH_PATH) {
                it.sendResponseHeaders(HttpProtocol.NOT_FOUND, -1)
                return
            }
            val request =
                try {
                    Json.mapper.readValue(it.requestBody, MatchRequest::class.java)
                } catch (error: IOException) {
                    it.sendResponseHeaders(HttpProtocol.BAD_REQUEST, -1)
                    return
                }
            try {
                val assignment = matches.match(request).toCompletableFuture().join()
                val body = Json.mapper.writeValueAsBytes(assignment)
                it.responseHeaders.set(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON)
                it.sendResponseHeaders(HttpProtocol.OK, body.size.toLong())
                it.responseBody.write(body)
            } catch (error: IllegalArgumentException) {
                it.sendResponseHeaders(HttpProtocol.BAD_REQUEST, -1)
            } catch (error: CompletionException) {
                error.cause?.printStackTrace(System.err)
                it.sendResponseHeaders(HttpProtocol.UNAVAILABLE, -1)
            }
        }
    }

    override fun close() {
        matches.close()
        server.stop(0)
        executor.close()
    }
}
