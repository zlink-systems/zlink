package systems.zlink.e2e.kotlin.registrationcodec.main.endpoints

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Instant
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.registrationcodec.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrationcodec.EvidenceWaitRes
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.ScenarioState

class EvidenceHttpServer(
    private val state: ScenarioState,
    private val json: ObjectMapper,
    private val endpoint: String,
    private val scenarioEndpoints: RegistrationScenarioEndpoints,
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
                exchange.responseBody.use { it.write(body) }
            }
            httpServer.createContext("/evidence") { exchange ->
                val body = json.writeValueAsBytes(state.snapshot())
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.use { it.write(body) }
            }
            httpServer.createContext("/evidence/wait") { exchange ->
                val request = exchange.requestBody.use { json.readValue(it, EvidenceWaitReq::class.java) }
                val deadline = Instant.now().plusMillis(request.timeoutMillis.coerceIn(1, 30_000))
                var entries = state.snapshot().entries
                while (Instant.now().isBefore(deadline) &&
                    entries.none {
                        it.marker == request.marker &&
                            it.packetName == request.packetName &&
                            it.value == request.value
                    }
                ) {
                    Thread.sleep(50)
                    entries = state.snapshot().entries
                }
                val body = json.writeValueAsBytes(EvidenceWaitRes(entries))
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.use { it.write(body) }
            }
            scenarioEndpoints.map(httpServer)
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
}
