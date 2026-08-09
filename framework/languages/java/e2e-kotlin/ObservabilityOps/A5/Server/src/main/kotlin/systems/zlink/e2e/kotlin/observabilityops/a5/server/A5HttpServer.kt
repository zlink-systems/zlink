package systems.zlink.e2e.kotlin.observabilityops.a5.server


import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.springframework.context.SmartLifecycle
import org.springframework.beans.factory.ObjectProvider
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.errors.ZLinkFrameworkException
import java.net.InetSocketAddress
import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import java.time.Duration

class A5HttpServer(
    private val json: ObjectMapper,
    private val runtimeOptions: ZLinkChannelRuntimeOptions,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
    private val routes: ZLinkRouteClient,
    private val evidence: FlowEvidence,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        val endpoint = URI.create(Env.get("e2e.http.endpoint"))
        server = HttpServer.create(InetSocketAddress(endpoint.host, endpoint.port), 0).apply {
            createContext("/health") { write(it, 200, "ok") }
            createContext("/evidence") { flowSnapshot(it) }
            createContext("/flows") { flowSnapshot(it) }
            createContext("/mode") { setMode(it) }
            createContext("/request") { request(it) }
            start()
        }
        running = true
    }

    private fun setMode(exchange: HttpExchange) {
        val mode = ZLinkMessageFlowLogMode.valueOf(query(exchange, "value"))
        runtime.getObject().setMessageFlowMode(mode)
        write(exchange, 200, json.writeValueAsString(Status(runtime.getObject().messageFlowMode().name, evidence.snapshot().size)))
    }

    private fun request(exchange: HttpExchange) {
        try {
            val reply = routes.requestToChannel(
                Contracts.CHANNEL,
                Contracts.ProbeRequest(query(exchange, "value"), query(exchange, "fail") == "true"),
            ).timeout(Duration.ofSeconds(5))
                .submit(Contracts.ProbeReply::class.java)
                .toCompletableFuture()
                .join()
            write(exchange, 200, json.writeValueAsString(reply))
        } catch (error: Throwable) {
            write(exchange, 500, failure(error))
        }
    }

    private fun flowSnapshot(exchange: HttpExchange) {
        val events = evidence.snapshot()
        val after = queryOrDefault(exchange, "after", "0").toInt()
        val start = after.coerceIn(0, events.size)
        write(exchange, 200, json.writeValueAsString(FlowSnapshot(
            events.size,
            events.subList(start, events.size),
        )))
    }

    private fun query(exchange: HttpExchange, name: String): String =
        queryOrDefault(exchange, name, "").takeIf { it.isNotBlank() }
            ?: error("missing query parameter $name")

    private fun queryOrDefault(exchange: HttpExchange, name: String, fallback: String): String {
        val raw = exchange.requestURI.rawQuery ?: return fallback
        return raw.split('&').asSequence()
            .map { it.split('=', limit = 2) }
            .firstOrNull { it.size == 2 && it[0] == name }
            ?.let { URLDecoder.decode(it[1], StandardCharsets.UTF_8) }
            ?: fallback
    }

    private fun failure(error: Throwable): String {
        val cause = error.cause ?: error
        return if (cause is ZLinkFrameworkException) {
            "${cause.kind().name}: ${cause.message}"
        } else {
            "${cause::class.java.name}: ${cause.message}"
        }
    }

    private fun write(exchange: HttpExchange, status: Int, body: String) {
        val bytes = body.toByteArray(StandardCharsets.UTF_8)
        exchange.responseHeaders.set("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, bytes.size.toLong())
        exchange.responseBody.use { it.write(bytes) }
    }

    override fun stop() { server?.stop(0); server = null; running = false }
    override fun isRunning() = running

    data class Status(val mode: String, val count: Int)
    data class FlowSnapshot(val count: Int, val events: List<FlowEvidence.FlowEvent>)
}
