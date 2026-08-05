package systems.zlink.e2e.kotlin.spotservice.session.endpoints

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Duration
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState

class SessionEvidenceHttpServer(
    private val state: ScenarioState,
    private val json: ObjectMapper,
    private val endpoint: String
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (endpoint.isBlank()) {
            return
        }
        try {
            val uri = URI.create(endpoint)
            val nextServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            nextServer.createContext("/health") { exchange ->
                write(exchange, 200, "ok\n")
            }
            nextServer.createContext("/evidence") { exchange ->
                val body = json.writeValueAsBytes(state.snapshot())
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.write(body)
                exchange.close()
            }
            nextServer.createContext("/evidence/wait") { exchange ->
                requirePost(exchange)
                val request = exchange.requestBody.use { body ->
                    json.readValue(body, Contracts.EvidenceWaitReq::class.java)
                }
                val snapshot = state.waitUntilContainsAll(
                    request.containsAll,
                    Duration.ofMillis(request.timeoutMilliseconds.coerceIn(1, 30_000).toLong())
                )
                val body = json.writeValueAsBytes(snapshot)
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.write(body)
                exchange.close()
            }
            nextServer.start()
            server = nextServer
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start session evidence endpoint $endpoint", error)
        }
    }

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean =
        running

    private fun write(exchange: HttpExchange, status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }

    private fun requirePost(exchange: HttpExchange) {
        if (exchange.requestMethod == "POST") {
            return
        }
        write(exchange, 405, "method not allowed\n")
        throw IllegalStateException("HTTP ${exchange.requestMethod} is not allowed for ${exchange.requestURI.path}")
    }
}
