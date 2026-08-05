package systems.zlink.e2e.kotlin.pubsub.subscriber

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import org.springframework.context.SmartLifecycle

class OperationalEndpoints(
    private val state: EvidenceStore,
    private val json: ObjectMapper,
    private val endpoint: String,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (endpoint.isBlank()) {
            return
        }
        try {
            val uri = URI.create(endpoint)
            val httpServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            httpServer.createContext("/health") { exchange ->
                val body = "ok\n".toByteArray(StandardCharsets.UTF_8)
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.write(body)
                exchange.close()
            }
            httpServer.createContext("/evidence") { exchange ->
                val body = json.writeValueAsBytes(state.snapshot())
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.write(body)
                exchange.close()
            }
            httpServer.createContext("/evidence/wait") { exchange ->
                try {
                    val query = parseQuery(exchange.requestURI.rawQuery.orEmpty())
                    val body = json.writeValueAsBytes(
                        state.waitFor(
                            marker = query["marker"],
                            scenario = query["scenario"],
                            sequence = query["sequence"]?.toIntOrNull(),
                            valueContains = query["contains"],
                            timeoutMillis = query["timeoutMs"]?.toLongOrNull() ?: 30_000L,
                        ),
                    )
                    exchange.responseHeaders.add("Content-Type", "application/json")
                    exchange.sendResponseHeaders(200, body.size.toLong())
                    exchange.responseBody.write(body)
                } catch (error: Exception) {
                    val body = "${error.message ?: error.javaClass.name}\n".toByteArray(StandardCharsets.UTF_8)
                    exchange.sendResponseHeaders(504, body.size.toLong())
                    exchange.responseBody.write(body)
                } finally {
                    exchange.close()
                }
            }
            httpServer.start()
            server = httpServer
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start evidence endpoint $endpoint", error)
        }
    }

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun parseQuery(query: String): Map<String, String> {
        if (query.isBlank()) {
            return emptyMap()
        }
        return query.split("&")
            .mapNotNull { part ->
                val index = part.indexOf("=")
                if (index <= 0) {
                    null
                } else {
                    val key = URLDecoder.decode(part.substring(0, index), StandardCharsets.UTF_8)
                    val value = URLDecoder.decode(part.substring(index + 1), StandardCharsets.UTF_8)
                    key to value
                }
            }
            .toMap()
    }
}
