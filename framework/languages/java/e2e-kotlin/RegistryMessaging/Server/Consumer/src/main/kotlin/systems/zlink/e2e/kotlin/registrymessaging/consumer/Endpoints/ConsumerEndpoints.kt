package systems.zlink.e2e.kotlin.registrymessaging.consumer.Endpoints

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
import systems.zlink.e2e.kotlin.registrymessaging.consumer.Configuration.ConsumerOptions
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.shared.BackpressureRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.PayloadRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.PayloadReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileMsg
import systems.zlink.e2e.kotlin.registrymessaging.shared.MissingProfileMsg
import systems.zlink.e2e.kotlin.registrymessaging.shared.MissingProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.RequestFailureRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowRes
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime
import systems.zlink.framework.monitoring.ZLinkPeerState
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class ConsumerEndpoints(
    private val options: ConsumerOptions,
    private val context: ConfigurableApplicationContext,
) {
    private val mapper = jacksonObjectMapper()
    private val channels = context.getBean(ZLinkClient::class.java)
    private val clientServerRuntime = context.getBean(ZLinkClientServerRuntime::class.java)

    fun start(): HttpServer {
        val uri = URI.create(options.httpUrl)
        val server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        server.executor = Executors.newCachedThreadPool()
        server.createContext("/health") { exchange ->
            exchange.writeJson(mapOf("status" to "ready", "role" to "consumer"))
        }
        server.createContext("/profile/batch-request") { exchange ->
            val requests = exchange.readJson<Array<ProfileReq>>()
            val replies = mutableListOf<ProfileRes>()
            var sequence: CompletionStage<Void> = CompletableFuture.completedFuture(null)
            requests.forEach { request ->
                sequence = sequence.thenCompose {
                    requestProfile(request, Duration.ofSeconds(5)).thenAccept(replies::add)
                }
            }
            exchange.writeJson(sequence.thenApply { replies.toList() })
        }
        server.createContext("/profile/request") { exchange ->
            exchange.writeJson(requestProfile(exchange.readJson(), Duration.ofSeconds(5)))
        }
        server.createContext("/workflow/request") { exchange ->
            val request = exchange.readJson<WorkflowReq>()
            val reply = channels.requestToChannel(Contracts.WORKFLOW_CHANNEL, request)
                .timeout(Duration.ofSeconds(5))
                .submit(WorkflowRes::class.java)
            exchange.writeJson(reply)
        }
        server.createContext("/locations/peers") { exchange ->
            val peers = CompletableFuture.completedFuture(
                clientServerRuntime.snapshot(Contracts.PROFILE_CHANNEL),
            ).thenApply { status -> status.targets
                .filter { it.state == ZLinkPeerState.READY }
                .map {
                    mapOf(
                        "meshName" to status.channelName,
                        "role" to "ROUTER",
                        "nodeRid" to it.nodeRid.toString(),
                        "state" to it.state.name.lowercase(),
                        "weight" to it.weight,
                        "endpoint" to "",
                        "ownerId" to "",
                    )
                } }
            exchange.writeJson(peers)
        }
        server.createContext("/profile/slow-request") { exchange ->
            val request = exchange.readJson<ProfileReq>()
            exchange.writeJson(requestProfile(request, Duration.ofMillis(100), retryUntilReady = false)
                .handle { _, error ->
                    if (error == null) RequestFailureRes(false, "")
                    else RequestFailureRes(true, rootName(error))
                })
        }
        server.createContext("/profile/missing-request") { exchange ->
            val request = exchange.readJson<MissingProfileReq>()
            exchange.writeJson(
                channels.requestToChannel(Contracts.PROFILE_CHANNEL, request)
                    .timeout(Duration.ofSeconds(5))
                    .submit(ProfileRes::class.java)
                    .handle { _, error ->
                        if (error == null) RequestFailureRes(false, "")
                        else RequestFailureRes(true, rootName(error))
                    },
            )
        }
        server.createContext("/profile/missing-command") { exchange ->
            val command = exchange.readJson<MissingProfileMsg>()
            channels.sendToChannel(Contracts.PROFILE_CHANNEL, command)
                .submit()
            exchange.writeJson(mapOf("status" to "sent"))
        }
        server.createContext("/profile/payload") { exchange ->
            exchange.writeJson(requestPayload(exchange.readJson()))
        }
        server.createContext("/profile/backpressure/reset") { exchange ->
            exchange.writeJson(mapOf("status" to "ready"))
        }
        server.createContext("/profile/backpressure/send") { exchange ->
            val command = exchange.readJson<ProfileMsg>()
            channels.sendToChannel(Contracts.PROFILE_CHANNEL, command)
                .submit()
            exchange.writeJson(BackpressureRes("Submitted"))
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

    private fun requestProfile(
        request: ProfileReq,
        timeout: Duration,
        retryUntilReady: Boolean = true,
    ): CompletionStage<ProfileRes> {
        val deadline = System.nanoTime() + Duration.ofSeconds(30).toNanos()
        return requestProfile(request, timeout, retryUntilReady, deadline)
    }

    private fun requestProfile(
        request: ProfileReq,
        timeout: Duration,
        retryUntilReady: Boolean,
        deadline: Long,
    ): CompletionStage<ProfileRes> = channels.requestToChannel(Contracts.PROFILE_CHANNEL, request)
        .timeout(timeout)
        .submit(ProfileRes::class.java)
        .handle { reply, error -> retryOrComplete(reply, error, retryUntilReady, deadline) {
            requestProfile(request, timeout, true, deadline)
        } }
        .thenCompose { it }

    private fun requestPayload(request: PayloadReq): CompletionStage<PayloadRes> =
        requestPayload(request, System.nanoTime() + Duration.ofSeconds(30).toNanos())

    private fun requestPayload(request: PayloadReq, deadline: Long): CompletionStage<PayloadRes> =
        channels.requestToChannel(Contracts.PROFILE_CHANNEL, request)
            .timeout(Duration.ofSeconds(10))
            .submit(PayloadRes::class.java)
            .handle { reply, error -> retryOrComplete(reply, error, true, deadline) {
                requestPayload(request, deadline)
            } }
            .thenCompose { it }

    private fun <T> retryOrComplete(
        value: T?,
        error: Throwable?,
        retry: Boolean,
        deadline: Long,
        next: () -> CompletionStage<T>,
    ): CompletionStage<T> {
        if (error == null) return CompletableFuture.completedFuture(value!!)
        if (!retry || System.nanoTime() >= deadline) return CompletableFuture.failedFuture(error)
        return CompletableFuture.runAsync({}, CompletableFuture.delayedExecutor(100, TimeUnit.MILLISECONDS))
            .thenCompose { next() }
    }

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

    private fun rootName(error: Throwable): String {
        var current = error
        while (current.cause != null) {
            current = current.cause!!
        }
        return current.javaClass.simpleName
    }
}
