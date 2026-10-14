package MyGame.accounts

import MyGame.*
import com.sun.net.httpserver.HttpServer
import java.io.File
import java.net.InetSocketAddress
import java.net.URI

fun main(args: Array<String>) {
    require(args.size == 1) { "Supply accounts configuration" }
    val config = Json.mapper.readValue(File(args.single()), NodeConfig::class.java)
    val endpoint = URI(requireNotNull(config.httpEndpoint))
    val server = HttpServer.create(InetSocketAddress(endpoint.host, endpoint.port), 0)
    val worlds = Json.mapper.writeValueAsBytes(config.worlds)
    server.createContext(Names.WORLD_LIST_PATH) { exchange ->
        exchange.use {
            if (it.requestMethod != "GET" || it.requestURI.path != Names.WORLD_LIST_PATH) {
                it.sendResponseHeaders(404, -1)
            } else {
                it.responseHeaders.set("Content-Type", "application/json")
                it.sendResponseHeaders(200, worlds.size.toLong())
                it.responseBody.write(worlds)
            }
        }
    }
    server.start()
    try {
        println(
            Control.READY +
                Json.mapper.writeValueAsString(
                    mapOf(
                        "id" to config.id,
                        "httpEndpoint" to "http://${endpoint.host}:${server.address.port}",
                    )
                )
        )
        val input = System.`in`.bufferedReader()
        while (true) {
            val command = input.readLine() ?: break
            require(command == Control.SHUTDOWN) { "Accounts supports shutdown only" }
            server.stop(0)
            println(Control.STOPPED)
            return
        }
    } finally {
        server.stop(0)
    }
}
