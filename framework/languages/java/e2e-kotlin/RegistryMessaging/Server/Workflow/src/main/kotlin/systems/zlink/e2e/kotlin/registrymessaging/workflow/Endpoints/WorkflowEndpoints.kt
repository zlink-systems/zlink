package systems.zlink.e2e.kotlin.registrymessaging.workflow.Endpoints

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.time.Duration
import java.util.concurrent.Executors
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import java.util.concurrent.CompletionStage
import java.util.concurrent.TimeUnit
import org.springframework.context.ConfigurableApplicationContext
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowReq
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Configuration.ServerOptions
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Infrastructure.EvidenceStore
import systems.zlink.framework.channels.ZLinkClient

class WorkflowEndpoints(
    private val options: ServerOptions,
    private val context: ConfigurableApplicationContext,
) {
    private val mapper = jacksonObjectMapper()
    private val evidence = context.getBean(EvidenceStore::class.java)
    private val channels = context.getBean(ZLinkClient::class.java)

    fun start(): HttpServer {
        val uri = URI.create(options.httpUrl)
        val server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        server.executor = Executors.newCachedThreadPool()
        server.createContext("/health") { exchange ->
            exchange.writeJson(mapOf("status" to "ready", "role" to "workflow", "rid" to options.rid))
        }
        server.createContext("/evidence") { exchange -> exchange.writeJson(evidence.snapshot()) }
        server.createContext("/evidence/clear") { exchange ->
            evidence.clear()
            exchange.writeJson(mapOf("status" to "cleared"))
        }
        server.createContext("/evidence/wait") { exchange ->
            val request = exchange.readJson<EvidenceWaitReq>()
            exchange.writeJson(
                evidence.waitUntil(
                    request.contains,
                    Duration.ofMillis(request.timeoutMilliseconds.coerceIn(1, 30000).toLong()),
                ),
            )
        }
        server.createContext("/workflow/request") { exchange ->
            val request = exchange.readJson<WorkflowReq>()
            exchange.writeJson(requestWorkflow(request))
        }
        server.createContext("/shutdown") { exchange ->
            exchange.writeJson(mapOf("status" to "stopping"))
            Thread {
                server.stop(0)
                context.close()
            }.start()
        }
        server.start()
        return server
    }

    private fun requestWorkflow(request: WorkflowReq): CompletionStage<WorkflowRes> =
        requestWorkflow(request, System.nanoTime() + Duration.ofSeconds(30).toNanos())

    private fun requestWorkflow(request: WorkflowReq, deadline: Long): CompletionStage<WorkflowRes> =
        channels.requestToChannel(Contracts.WORKFLOW_CHANNEL, request)
            .timeout(Duration.ofSeconds(5))
            .submit(WorkflowRes::class.java)
            .handle { reply, error ->
                if (error == null) {
                    CompletableFuture.completedFuture(reply)
                } else if (System.nanoTime() >= deadline) {
                    CompletableFuture.failedFuture(error)
                } else {
                    CompletableFuture.runAsync({}, CompletableFuture.delayedExecutor(100, TimeUnit.MILLISECONDS))
                        .thenCompose { requestWorkflow(request, deadline) }
                }
            }
            .thenCompose { it }

    private inline fun <reified T> HttpExchange.readJson(): T =
        requestBody.use { mapper.readValue(it) }

    private fun HttpExchange.writeJson(value: Any) {
        try {
            val bytes = mapper.writeValueAsBytes(value)
            responseHeaders.add("content-type", "application/json")
            sendResponseHeaders(200, bytes.size.toLong())
            responseBody.use { it.write(bytes) }
        } catch (error: Exception) {
            val bytes = mapper.writeValueAsBytes(mapOf("error" to (error.message ?: error.javaClass.name)))
            sendResponseHeaders(500, bytes.size.toLong())
            responseBody.use { it.write(bytes) }
        }
    }

    private fun HttpExchange.writeJson(value: CompletionStage<*>) {
        value.whenComplete { result, error ->
            if (error == null) writeJson(result ?: mapOf<String, String>())
            else writeJson(mapOf("error" to (unwrap(error).message ?: unwrap(error).javaClass.name)))
        }
    }

    private fun unwrap(error: Throwable): Throwable =
        if (error is CompletionException && error.cause != null) error.cause!! else error
}
