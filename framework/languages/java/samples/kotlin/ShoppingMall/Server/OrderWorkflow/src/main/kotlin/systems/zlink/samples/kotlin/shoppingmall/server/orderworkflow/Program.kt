package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Duration
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology

fun main(args: Array<String>) {
    val app = OrderWorkflowApplication.run(SampleTopology.configPath(args))
    val topology = app.getBean(SampleTopology::class.java)
    val http = startHttp(topology, app.getBean(ZLinkFrameworkRuntime::class.java))
    Runtime.getRuntime().addShutdownHook(Thread {
        http.stop(0)
        app.close()
    })
    Thread.currentThread().join()
}

private fun startHttp(topology: SampleTopology, runtime: ZLinkFrameworkRuntime): HttpServer {
    val json = jacksonObjectMapper()
    val endpoint = URI.create(topology.role().httpEndpoint)
    val server = HttpServer.create(InetSocketAddress(endpoint.host, endpoint.port), 0)
    server.createContext("/health") { exchange -> exchange.writeJson(json, 200, mapOf("status" to "ok")) }
    server.createContext("/self-check/relocate") { exchange ->
        if (exchange.requestMethod != "POST") {
            exchange.writeJson(json, 405, mapOf("error" to "method not allowed"))
        } else {
            try {
                val result = runtime.relocate(
                    ZLinkFrameworkRelocationOptions(
                        ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                        null,
                        Duration.ofSeconds(30),
                    ),
                ).toCompletableFuture().join()
                exchange.writeJson(json, 200, mapOf("outcome" to result.outcome().name))
            } catch (error: Throwable) {
                exchange.writeJson(json, 500, mapOf("error" to (error.message ?: error.javaClass.simpleName)))
            }
        }
    }
    server.start()
    return server
}

private fun HttpExchange.writeJson(
    json: com.fasterxml.jackson.databind.ObjectMapper,
    status: Int,
    body: Any,
) {
    val bytes = json.writeValueAsString(body).toByteArray(StandardCharsets.UTF_8)
    responseHeaders.add("content-type", "application/json")
    sendResponseHeaders(status, bytes.size.toLong())
    responseBody.use { it.write(bytes) }
}
