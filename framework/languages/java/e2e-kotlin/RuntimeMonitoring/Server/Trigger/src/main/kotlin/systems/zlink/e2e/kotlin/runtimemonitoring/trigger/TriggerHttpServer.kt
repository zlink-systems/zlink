package systems.zlink.e2e.kotlin.runtimemonitoring.trigger

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.Env
import systems.zlink.e2e.kotlin.runtimemonitoring.service.EvidenceState
import systems.zlink.framework.channels.ZLinkClient
import org.springframework.boot.WebApplicationType
import org.springframework.boot.builder.SpringApplicationBuilder
import systems.zlink.e2e.kotlin.runtimemonitoring.trigger.transient.TransientTriggerApplication
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Duration

class TriggerHttpServer(
    private val endpoint: String,
    private val state: EvidenceState,
    private val json: ObjectMapper,
    private val client: ZLinkClient,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        try {
            val uri = URI.create(endpoint)
            val nextServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            nextServer.createContext("/health") { exchange -> write(exchange, "ok\n") }
            nextServer.createContext("/evidence") { exchange ->
                write(exchange, json.writeValueAsString(state.snapshot()), "application/json")
            }
            nextServer.createContext("/profile/request") { exchange ->
                write(exchange, requestService(), "application/json")
            }
            nextServer.createContext("/profile/request/disconnect") { exchange ->
                write(exchange, requestTransientService("mon-a1-request"), "application/json")
            }
            nextServer.createContext("/profile/request/service-b") { exchange ->
                write(
                    exchange,
                    requestTransient(
                        Env.get("e2e.filtered.api.endpoint"),
                        exchange.query("value") ?: "mon-d1-request",
                    ),
                    "application/json",
                )
            }
            nextServer.createContext("/profile/request/throw") { exchange ->
                write(
                    exchange,
                    requestTransient(
                        Env.get("e2e.throwing.api.endpoint"),
                        exchange.query("value") ?: "mon-c1-request",
                    ),
                    "application/json",
                )
            }
            nextServer.start()
            server = nextServer
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start trigger endpoint $endpoint", error)
        }
    }

    private fun requestService(): String {
        val reply = client.requestToChannel(
            Contracts.CHANNEL,
            Contracts.WorkReq("mon-a4-trigger"),
        )
            .timeout(Duration.ofSeconds(3))
            .submit(Contracts.WorkRes::class.java).toCompletableFuture().get()
        return "{\"providerRid\":\"${reply.providerRid}\",\"value\":\"${reply.value}\"}\n"
    }

    private fun requestServiceB(): String {
        return requestTransient(Env.get("e2e.filtered.api.endpoint"), "mon-d1-request")
    }

    private fun requestTransientService(value: String): String {
        return requestTransient(Env.get("e2e.service.api.endpoint"), value)
    }

    private fun requestTransient(endpoint: String, value: String): String {
        val context = SpringApplicationBuilder(TransientTriggerApplication::class.java)
            .web(WebApplicationType.NONE)
            .properties(mapOf("zlink.e2e.transient-endpoint" to endpoint))
            .run()
        return context.use {
            val transientClient = context.getBean(ZLinkClient::class.java)
            val reply = transientClient.requestToChannel(
                Contracts.CHANNEL,
                Contracts.WorkReq(value),
            )
                .timeout(Duration.ofSeconds(3))
                .submit(Contracts.WorkRes::class.java).toCompletableFuture().get()
            "{\"providerRid\":\"${reply.providerRid}\",\"value\":\"${reply.value}\"}\n"
        }
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
        contentType: String = "text/plain; charset=utf-8",
    ) {
        val path = exchange.requestURI.path
        val getAllowed = exchange.requestMethod == "GET" && (path == "/health" || path == "/evidence")
        val postAllowed = exchange.requestMethod == "POST"
        if (!getAllowed && !postAllowed) {
            exchange.sendResponseHeaders(405, -1)
            exchange.close()
            return
        }

        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.responseHeaders.add("Content-Type", contentType)
        exchange.sendResponseHeaders(200, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }

    private fun HttpExchange.query(name: String): String? =
        requestURI.rawQuery
            ?.split("&")
            ?.mapNotNull {
                val index = it.indexOf('=')
                if (index < 0) null else it.substring(0, index) to it.substring(index + 1)
            }
            ?.firstOrNull { it.first == name }
            ?.second
}
