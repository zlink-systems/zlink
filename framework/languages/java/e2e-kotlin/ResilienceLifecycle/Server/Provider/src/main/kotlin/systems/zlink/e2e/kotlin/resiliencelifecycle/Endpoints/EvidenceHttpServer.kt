package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.SmartLifecycle
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions

class EvidenceHttpServer(
    private val state: ScenarioState,
    private val json: ObjectMapper,
    private val endpoint: String?,
    private val runtimeOptions: ZLinkChannelRuntimeOptions,
    private val applicationContext: ConfigurableApplicationContext,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (endpoint.isNullOrBlank()) {
            return
        }
        try {
            val uri = URI.create(endpoint)
            val httpServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            httpServer.createContext("/health") { exchange -> write(exchange, 200, "ok\n") }
            httpServer.createContext("/evidence") { exchange ->
                val body = json.writeValueAsBytes(state.snapshot())
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.use { it.write(body) }
            }
            httpServer.createContext("/admin/drain") { exchange -> setWeight(exchange, 0, "drained") }
            httpServer.createContext("/admin/restore") { exchange -> setWeight(exchange, 100, "restored") }
            httpServer.createContext("/admin/weight") { exchange ->
                write(exchange, 200, "{\"weight\":${state.weight()}}\n")
            }
            httpServer.createContext("/admin/release-slow") { exchange ->
                state.releaseSlow()
                write(exchange, 200, "{\"status\":\"released\"}\n")
            }
            httpServer.createContext("/admin/fault-on") { exchange -> setGrayFailure(exchange, true) }
            httpServer.createContext("/admin/fault-off") { exchange -> setGrayFailure(exchange, false) }
            httpServer.createContext("/admin/fault/observer-throws") { exchange ->
                state.observerThrows(true)
                state.record("ObserverFaultMode", "observer-throws")
                write(exchange, 200, "{\"observerThrows\":true}\n")
            }
            httpServer.createContext("/admin/fault/none") { exchange ->
                state.grayFailure(false)
                state.observerThrows(false)
                state.record("ObserverFaultMode", "none")
                write(exchange, 200, "{\"observerThrows\":false,\"grayFailure\":false}\n")
            }
            httpServer.createContext("/admin/shutdown") { exchange ->
                state.record("AdminShutdown", state.providerRid())
                write(exchange, 200, "{\"status\":\"shutting-down\"}\n")
                val shutdown = Thread(applicationContext::close, "kotlin-rl-admin-shutdown")
                shutdown.isDaemon = false
                shutdown.start()
            }
            httpServer.start()
            server = httpServer
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start evidence endpoint $endpoint", error)
        }
    }

    private fun setGrayFailure(exchange: HttpExchange, enabled: Boolean) {
        state.grayFailure(enabled)
        state.record("GrayFailureMode", enabled.toString())
        write(exchange, 200, "{\"grayFailure\":$enabled}\n")
    }

    private fun setWeight(exchange: HttpExchange, weight: Int, status: String) {
        runtimeOptions.clientServerChannel(Contracts.CHANNEL)
            .configureServerSocket()
            .weight(weight)
        state.weight(weight)
        state.record("AdminWeight", weight.toString())
        write(exchange, 200, "{\"status\":\"$status\",\"weight\":$weight}\n")
    }

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun write(exchange: HttpExchange, status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.use { it.write(body) }
    }
}
