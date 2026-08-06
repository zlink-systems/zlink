package systems.zlink.e2e.kotlin.pubsub.subscriber

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import org.springframework.beans.factory.ObjectProvider
import org.springframework.context.SmartLifecycle
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime

class OperationalEndpoints(
    private val state: EvidenceStore,
    private val json: ObjectMapper,
    private val endpoint: String,
    private val connections: SubscriberConnections,
    private val observers: FanoutObserverController,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
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
            httpServer.createContext("/status") { exchange ->
                exchange.writeJson(fanoutStatus())
            }
            httpServer.createContext("/connections") { exchange ->
                try {
                    val query = parseQuery(exchange.requestURI.rawQuery.orEmpty())
                    val operation = query["operation"] ?: "list"
                    when (operation) {
                        "connect" -> connections.connect(required(query, "endpoint"))
                        "disconnect" -> connections.disconnect(required(query, "endpoint"))
                        "list" -> Unit
                        else -> throw IllegalArgumentException("unsupported connection operation: $operation")
                    }
                    exchange.writeJson(mapOf("connections" to connections.list()))
                } catch (error: Exception) {
                    exchange.writeText(400, "${error.message ?: error.javaClass.name}\n")
                }
            }
            httpServer.createContext("/observer/start") { exchange ->
                try {
                    val query = parseQuery(exchange.requestURI.rawQuery.orEmpty())
                    observers.start(
                        name = query["name"] ?: "normal",
                        capacity = query["capacity"]?.toIntOrNull() ?: 1,
                        slow = query["slow"]?.toBoolean() ?: false,
                    )
                    exchange.writeJson(mapOf("status" to "started"))
                } catch (error: Exception) {
                    exchange.writeText(400, "${error.message ?: error.javaClass.name}\n")
                }
            }
            httpServer.createContext("/observer/release") { exchange ->
                val query = parseQuery(exchange.requestURI.rawQuery.orEmpty())
                observers.release(query["name"] ?: "slow")
                exchange.writeJson(mapOf("status" to "released"))
            }
            httpServer.createContext("/observer/cancel") { exchange ->
                val query = parseQuery(exchange.requestURI.rawQuery.orEmpty())
                observers.cancel(query["name"] ?: "slow")
                exchange.writeJson(mapOf("status" to "cancelled"))
            }
            httpServer.createContext("/observer/wait") { exchange ->
                try {
                    val query = parseQuery(exchange.requestURI.rawQuery.orEmpty())
                    observers.waitFor(
                        query["name"] ?: "normal",
                        query["timeoutMs"]?.toLongOrNull() ?: 30_000L,
                    )
                    exchange.writeJson(mapOf("status" to "observed"))
                } catch (error: Exception) {
                    exchange.writeText(504, "${error.message ?: error.javaClass.name}\n")
                }
            }
            httpServer.createContext("/observer/evidence") { exchange ->
                exchange.writeJson(observers.snapshot())
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

    private fun fanoutStatus(): Map<String, Any?> {
        val status = runtime.getObject().fanoutRuntime().snapshot(systems.zlink.e2e.kotlin.pubsub.shared.Contracts.EVENT_CHANNEL)
        return mapOf(
            "channelName" to status.channelName(),
            "state" to status.state().name,
            "isReady" to status.isReady,
            "readyPublisherCount" to status.readyPublisherCount(),
            "publishers" to status.publishers().map { publisher ->
                mapOf(
                    "nodeRid" to publisher.nodeRid().toString(),
                    "state" to publisher.state().name,
                    "unavailableReason" to publisher.unavailableReason().map { it.name }.orElse(null),
                )
            },
            "sequence" to status.sequence(),
            "observedAt" to status.observedAt().toString(),
        )
    }

    private fun required(query: Map<String, String>, name: String): String =
        query[name]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("$name is required")

    private fun com.sun.net.httpserver.HttpExchange.writeJson(value: Any) {
        val body = json.writeValueAsBytes(value)
        responseHeaders.add("Content-Type", "application/json")
        sendResponseHeaders(200, body.size.toLong())
        responseBody.use { it.write(body) }
    }

    private fun com.sun.net.httpserver.HttpExchange.writeText(status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        sendResponseHeaders(status, body.size.toLong())
        responseBody.use { it.write(body) }
    }
}
