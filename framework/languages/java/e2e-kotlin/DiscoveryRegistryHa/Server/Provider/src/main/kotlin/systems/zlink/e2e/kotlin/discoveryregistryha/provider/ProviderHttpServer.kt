package systems.zlink.e2e.kotlin.discoveryregistryha.provider

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import kotlin.system.exitProcess
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.discoveryregistryha.ProviderOptions
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.Support.ProviderEvidenceStore

class ProviderHttpServer(
    private val options: ProviderOptions,
    private val evidence: ProviderEvidenceStore,
    private val json: ObjectMapper,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (options.httpEndpoint().isNullOrBlank()) {
            return
        }
        val uri = URI.create(options.httpEndpoint())
        val httpServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        httpServer.createContext("/health") { exchange ->
            write(exchange, "ok\n", contentType = "text/plain")
        }
        httpServer.createContext("/evidence") { exchange ->
            write(exchange, json.writeValueAsString(mapOf("providerRid" to evidence.providerRid)))
        }
        httpServer.createContext("/shutdown") { exchange ->
            write(exchange, """{"status":"stopping"}""")
            Thread { exitProcess(0) }.start()
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

    private fun write(exchange: HttpExchange, value: String, status: Int = 200, contentType: String = "application/json") {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.responseHeaders.add("Content-Type", contentType)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.use { stream -> stream.write(body) }
        exchange.close()
    }
}
