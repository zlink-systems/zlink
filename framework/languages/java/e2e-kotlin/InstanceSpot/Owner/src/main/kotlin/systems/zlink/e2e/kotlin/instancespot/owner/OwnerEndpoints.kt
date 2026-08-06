package systems.zlink.e2e.kotlin.instancespot.owner

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpServer
import java.util.concurrent.Executors
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.instancespot.shared.Contracts
import systems.zlink.e2e.kotlin.instancespot.shared.HttpSupport

class OwnerEndpoints(
    private val options: OwnerOptions,
    private val evidence: EvidenceStore,
    private val gates: GateController,
    private val json: ObjectMapper,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var executor: java.util.concurrent.ExecutorService? = null

    override fun start() {
        val http = HttpSupport.server(options.httpEndpoint)
        executor = Executors.newFixedThreadPool(4)
        http.executor = executor
        http.createContext("/health") { exchange ->
            HttpSupport.write(exchange, json, mapOf("status" to "ready", "rid" to options.rid, "lifecycleId" to options.lifecycleId))
        }
        http.createContext("/evidence") { exchange -> HttpSupport.write(exchange, json, evidence.snapshot()) }
        http.createContext("/evidence/wait") { exchange ->
            HttpSupport.write(exchange, json, evidence.waitFor(HttpSupport.read(exchange, json, Contracts.EvidenceWaitRequest::class.java)))
        }
        http.createContext("/gate") { exchange ->
            val request = HttpSupport.read(exchange, json, Contracts.GateRequest::class.java)
            gates.set(request.gateId, request.open)
            HttpSupport.write(exchange, json, request)
        }
        http.createContext("/shutdown") { exchange ->
            HttpSupport.write(exchange, json, mapOf("status" to "stopping"))
            HttpSupport.shutdownAsync()
        }
        http.start()
        server = http
    }

    override fun stop() {
        server?.stop(0)
        server = null
        executor?.shutdownNow()
        executor = null
    }

    override fun isRunning(): Boolean = server != null
}
