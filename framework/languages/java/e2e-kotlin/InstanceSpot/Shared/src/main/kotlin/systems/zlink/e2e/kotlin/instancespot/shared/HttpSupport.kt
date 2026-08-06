package systems.zlink.e2e.kotlin.instancespot.shared

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets

object HttpSupport {
    fun server(endpoint: String): HttpServer {
        val uri = URI.create(endpoint)
        return HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
    }

    fun body(exchange: HttpExchange): String = exchange.requestBody.use {
        it.readBytes().toString(StandardCharsets.UTF_8)
    }

    fun <T> read(exchange: HttpExchange, json: ObjectMapper, type: Class<T>): T =
        json.readValue(body(exchange), type)

    fun write(exchange: HttpExchange, json: ObjectMapper, value: Any, status: Int = 200) {
        val bytes = json.writeValueAsBytes(value)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, bytes.size.toLong())
        exchange.responseBody.use { it.write(bytes) }
    }

    fun shutdownAsync() {
        Thread {
            Thread.sleep(100)
            kotlin.system.exitProcess(0)
        }.apply { isDaemon = false; name = "zlink-kotlin-instance-spot-shutdown" }.start()
    }
}
