package systems.zlink.e2e.kotlin.pubsub.publisher

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import org.springframework.beans.factory.ObjectProvider
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.e2e.kotlin.pubsub.shared.Event
import systems.zlink.e2e.kotlin.pubsub.shared.MissingEvent
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.monitoring.ZLinkListenerKind
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime

class PublisherEndpoints(
    private val fanout: ZLinkFanoutClient,
    private val json: ObjectMapper,
    private val endpoint: String,
    private val channelName: String,
    private val application: ConfigurableApplicationContext,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (endpoint.isBlank()) {
            return
        }
        val uri = URI.create(endpoint)
        val httpServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        httpServer.createContext("/health") { exchange ->
            val body = "ok\n".toByteArray(StandardCharsets.UTF_8)
            exchange.sendResponseHeaders(200, body.size.toLong())
            exchange.responseBody.write(body)
            exchange.close()
        }
        httpServer.createContext("/publish") { exchange ->
            val request = exchange.readJson<PublishReq>()
            fanout.publish(channelName, request.topic, request.message)
                .submit()
                .toCompletableFuture()
                .join()
            exchange.writeJson(mapOf("status" to "published"))
        }
        httpServer.createContext("/publish-missing") { exchange ->
            val request = exchange.readJson<PublishReq>()
            fanout.publish(
                channelName,
                request.topic,
                MissingEvent(request.message.scenario, request.message.sequence, request.message.value),
            ).submit().toCompletableFuture().join()
            exchange.writeJson(mapOf("status" to "published"))
        }
        httpServer.createContext("/publish-reserved") { exchange ->
            try {
                fanout.publish(channelName, "\u0001ZLF1", Event("ps-f3", 1, "reserved"))
                    .submit()
                    .toCompletableFuture()
                    .join()
                exchange.writeJson(mapOf("status" to "accepted"))
            } catch (error: Exception) {
                val body = json.writeValueAsBytes(
                    mapOf(
                        "error" to (error.message ?: error.javaClass.simpleName),
                        "type" to error.javaClass.simpleName,
                    ),
                )
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(400, body.size.toLong())
                exchange.responseBody.use { it.write(body) }
            } finally {
                exchange.close()
            }
        }
        httpServer.createContext("/publish-reserved-prefix") { exchange ->
            fanout.publish(channelName, "\u0001ZLF1.more", Event("ps-f3", 2, "reserved-prefix"))
                .submit()
                .toCompletableFuture()
                .join()
            exchange.writeJson(mapOf("status" to "published"))
        }
        httpServer.createContext("/status") { exchange ->
            val status = runtime.getObject().fanoutRuntime().snapshot(channelName)
            val listener = runtime.getObject().listenerStatus(ZLinkListenerKind.FANOUT, channelName)
            exchange.writeJson(
                mapOf(
                    "channelName" to status.channelName(),
                    "state" to status.state().name,
                    "isReady" to status.isReady,
                    "readyPublisherCount" to status.readyPublisherCount(),
                    "publishers" to status.publishers().map { publisher ->
                        mapOf(
                            "nodeRid" to publisher.nodeRid().toString(),
                            "state" to publisher.state().name,
                        )
                    },
                    "sequence" to status.sequence(),
                    "observedAt" to status.observedAt().toString(),
                    "listenerEndpoint" to listener.endpoint(),
                ),
            )
        }
        httpServer.createContext("/shutdown") { exchange ->
            val body = "stopping\n".toByteArray(StandardCharsets.UTF_8)
            exchange.sendResponseHeaders(200, body.size.toLong())
            exchange.responseBody.write(body)
            exchange.close()
            Thread {
                Thread.sleep(100)
                application.close()
            }.start()
        }
        httpServer.start()
        server = httpServer
        running = true
    }

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private inline fun <reified T> HttpExchange.readJson(): T =
        requestBody.use { json.readValue(it) }

    private fun HttpExchange.writeJson(value: Any) {
        val body = json.writeValueAsBytes(value)
        responseHeaders.add("Content-Type", "application/json")
        sendResponseHeaders(200, body.size.toLong())
        responseBody.write(body)
        close()
    }
}

class PublishReq() {
    var topic: String = ""
    var message: Event = Event()

    constructor(topic: String, message: Event) : this() {
        this.topic = topic
        this.message = message
    }
}
