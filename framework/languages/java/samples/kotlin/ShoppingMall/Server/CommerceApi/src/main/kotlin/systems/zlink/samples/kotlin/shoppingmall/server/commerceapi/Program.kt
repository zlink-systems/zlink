package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.runBlocking
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers.ServerAssertionHandler
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.GetOrderStateRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildProjectionApiRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderRes

fun main(args: Array<String>) {
    val app = CommerceApiApplication.run(SampleTopology.configPath(args))
    val topology = app.getBean(SampleTopology::class.java)
    val http = startHttp(
        topology,
        app.getBean(StartOrderUseCase::class.java),
        app.getBean(OrderWorkflowRouter::class.java),
        app.getBean(CommerceStore::class.java),
        app.getBean(ServerAssertionHandler::class.java),
        app.getBean(ZLinkRouteMeshRuntime::class.java),
    )
    Runtime.getRuntime().addShutdownHook(Thread {
        http.stop(0)
        app.close()
    })
    Thread.currentThread().join()
}

private fun startHttp(
    topology: SampleTopology,
    starts: StartOrderUseCase,
    workflows: OrderWorkflowRouter,
    store: CommerceStore,
    assertions: ServerAssertionHandler,
    meshes: ZLinkRouteMeshRuntime,
): HttpServer {
    val json = jacksonObjectMapper()
    val role = topology.role()
    val endpoint = URI.create(role.httpEndpoint)
    val server = HttpServer.create(InetSocketAddress(endpoint.host, endpoint.port), 0)
    server.createContext("/health") { exchange -> exchange.writeJson(json, 200, mapOf("status" to "ok")) }
    server.createContext("/orders/start") { exchange ->
        if (exchange.requestMethod != "POST") return@createContext exchange.writeJson(json, 405, mapOf("error" to "method not allowed"))
        exchange.runSafely(json) { starts.execute(json.readValue(exchange.requestBody, StartOrderReq::class.java)) }
    }
    server.createContext("/orders/") { exchange ->
        val parts = exchange.requestURI.path.split("/")
        val orderId = parts.getOrElse(2) { "" }
        exchange.runSafely(json) {
            when {
                exchange.requestMethod == "GET" && parts.size == 3 ->
                    GetOrderStateRes(store.findReadModel(orderId) ?: error("Order '$orderId' does not exist."))
                exchange.requestMethod == "POST" && parts.getOrElse(3) { "" } == "continue" ->
                    ContinueOrderWorkflowRes(workflows.continueWorkflow(orderId))
                exchange.requestMethod == "POST" && parts.getOrElse(3) { "" } == "rebuild" ->
                    RebuildProjectionApiRes(workflows.rebuildProjection(orderId))
                else -> throw IllegalArgumentException("Unknown public order API path.")
            }
        }
    }
    server.createContext("/self-check/idempotency/pending") { exchange ->
        exchange.runSafely(json) {
            val request = json.readValue(exchange.requestBody, StartOrderReq::class.java)
            val mapping = store.reserveIdempotency(request.idempotencyKey, role.instanceId)
            StartOrderRes(mapping.orderId, "Created")
        }
    }
    server.createContext("/self-check/workflow/inventory-reserved") { exchange ->
        exchange.runSafely(json) { starts.prepareInventoryReserved(json.readValue(exchange.requestBody, StartOrderReq::class.java)) }
    }
    server.createContext("/self-check/workflow/") { exchange ->
        val orderId = exchange.requestURI.path.split("/").getOrElse(3) { "" }
        exchange.runSafely(json) { workflows.continueWorkflow(orderId) }
    }
    server.createContext("/self-check/projection/") { exchange ->
        val parts = exchange.requestURI.path.split("/")
        val orderId = parts.getOrElse(3) { "" }
        exchange.runSafely(json) {
            when (parts.getOrElse(4) { "" }) {
                "delete" -> mapOf("deleted" to store.deleteReadModel(orderId))
                "rebuild" -> RebuildProjectionApiRes(workflows.rebuildProjection(orderId))
                else -> throw IllegalArgumentException("Unknown projection self-check path.")
            }
        }
    }
    server.createContext("/self-check/assert") { exchange ->
        exchange.runSafely(json) { assertions.assert(json.readValue(exchange.requestBody, ServerAssertionReq::class.java)) }
    }
    server.start()
    println("shoppingmall-ready kind=http node=${role.instanceId}")
    startObjectRouteReadiness(role.instanceId, meshes)
    return server
}

private fun startObjectRouteReadiness(nodeId: String, meshes: ZLinkRouteMeshRuntime) {
    val readiness = Executors.newSingleThreadScheduledExecutor { task ->
        Thread(task, "shoppingmall-object-route-readiness-$nodeId").apply { isDaemon = true }
    }
    val attempts = AtomicInteger()
    readiness.scheduleWithFixedDelay({
        val attempt = attempts.incrementAndGet()
        try {
            if (meshes.isReady(SampleNames.OrderWorkflowMesh)) {
                println("shoppingmall-ready kind=object-route node=$nodeId target=workflow-a")
                println("shoppingmall-ready kind=object-route node=$nodeId target=workflow-b")
                readiness.shutdown()
            }
        } catch (_: IllegalStateException) {
            // The passive runtime view becomes available after Framework startup.
        } finally {
            if (attempt >= 300) readiness.shutdown()
        }
    }, 0, 100, TimeUnit.MILLISECONDS)
}

private fun HttpExchange.runSafely(json: ObjectMapper, action: suspend () -> Any) {
    try {
        writeJson(json, 200, runBlocking { action() })
    } catch (error: Throwable) {
        writeJson(json, 500, mapOf("error" to (error.message ?: error.javaClass.simpleName)))
    }
}

private fun HttpExchange.writeJson(json: ObjectMapper, status: Int, body: Any) {
    val bytes = json.writeValueAsBytes(body)
    responseHeaders.add("content-type", "application/json")
    sendResponseHeaders(status, bytes.size.toLong())
    responseBody.use { it.write(bytes) }
}
