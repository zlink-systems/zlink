package systems.zlink.e2e.kotlin.pubsub.publisher

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.e2e.kotlin.pubsub.shared.EventMsg
import systems.zlink.e2e.kotlin.pubsub.shared.MissingEventMsg
import systems.zlink.framework.channels.ZLinkFanoutClient

class PublisherEndpoints(
    private val fanout: ZLinkFanoutClient,
    private val json: ObjectMapper,
    private val endpoint: String,
    private val application: ConfigurableApplicationContext,
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
            fanout.publish(Contracts.EVENT_CHANNEL, request.topic, request.message)
                .submit()
                .toCompletableFuture()
                .join()
            exchange.writeJson(mapOf("status" to "published"))
        }
        httpServer.createContext("/publish-missing") { exchange ->
            val request = exchange.readJson<PublishReq>()
            fanout.publish(
                Contracts.EVENT_CHANNEL,
                request.topic,
                MissingEventMsg(request.message.scenario, request.message.sequence, request.message.value),
            ).submit().toCompletableFuture().join()
            exchange.writeJson(mapOf("status" to "published"))
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
    var message: EventMsg = EventMsg()

    constructor(topic: String, message: EventMsg) : this() {
        this.topic = topic
        this.message = message
    }
}
