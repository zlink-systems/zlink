package systems.zlink.e2e.kotlin.runtimemonitoring.service

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets

class EvidenceHttpServer @JvmOverloads constructor(
    private val state: EvidenceState,
    private val json: ObjectMapper,
    private val endpoint: String?,
    private val runtimeOptions: ZLinkChannelRuntimeOptions? = null,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (endpoint.isNullOrBlank()) {
            return
        }
        try {
            val uri = URI.create(endpoint)
            val nextServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            nextServer.createContext("/health") { exchange -> write(exchange, "ok\n") }
            nextServer.createContext("/evidence") { exchange ->
                write(exchange, json.writeValueAsString(state.snapshot()))
            }
            nextServer.createContext("/shutdown") { exchange ->
                if (exchange.requestMethod != "POST") {
                    exchange.sendResponseHeaders(405, -1)
                    exchange.close()
                    return@createContext
                }
                write(exchange, "{\"status\":\"shutting-down\"}\n")
                Thread({ System.exit(0) }, "runtime-monitoring-shutdown").apply {
                    isDaemon = false
                    start()
                }
            }
            if (runtimeOptions != null) {
                nextServer.createContext("/admin/drain") { exchange -> setWeight(exchange, 0, "drained") }
                nextServer.createContext("/admin/restore") { exchange -> setWeight(exchange, 100, "restored") }
            }
            nextServer.start()
            server = nextServer
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start evidence endpoint $endpoint", error)
        }
    }

    private fun setWeight(
        exchange: HttpExchange,
        weight: Int,
        status: String,
    ) {
        if (exchange.requestMethod != "POST") {
            exchange.sendResponseHeaders(405, -1)
            exchange.close()
            return
        }
        runtimeOptions
            ?.clientServerChannel(Contracts.CHANNEL)
            ?.configureServerSocket()
            ?.weight(weight)
        state.record("admin", Contracts.CHANNEL, "WeightChanged", "status=$status|weight=$weight")
        write(exchange, "{\"status\":\"$status\",\"weight\":$weight}\n")
    }

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun write(
        exchange: HttpExchange,
        value: String,
    ) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(200, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }
}
