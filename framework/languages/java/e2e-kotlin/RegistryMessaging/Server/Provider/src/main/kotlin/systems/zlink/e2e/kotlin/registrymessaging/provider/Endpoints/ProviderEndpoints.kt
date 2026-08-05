package systems.zlink.e2e.kotlin.registrymessaging.provider.Endpoints

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
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.registrymessaging.provider.Configuration.ServerOptions
import systems.zlink.e2e.kotlin.registrymessaging.provider.Infrastructure.EvidenceStore
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.shared.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileMsg
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.RouteMissingRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ScenarioRoutePingReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ScenarioRoutePingRes
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class ProviderEndpoints(
    private val options: ServerOptions,
    private val context: ConfigurableApplicationContext,
) {
    private val mapper = jacksonObjectMapper()
    private val evidence = context.getBean(EvidenceStore::class.java)
    private val channels = context.getBean(ZLinkClient::class.java)
    private val routes = context.getBean(ZLinkRouteClient::class.java)
    private val lifecycle = context.getBean(ZLinkFrameworkLifecycle::class.java)

    fun start(): HttpServer {
        val uri = URI.create(options.httpUrl)
        val server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        server.executor = Executors.newCachedThreadPool()
        server.createContext("/health") { exchange ->
            exchange.writeJson(mapOf("status" to "ready", "role" to "provider", "rid" to options.rid))
        }
        server.createContext("/route/status") { exchange ->
            val snapshot = lifecycle.routeMeshRuntime().snapshot(Contracts.PROFILE_ROUTE_CHANNEL)
            exchange.writeJson(
                mapOf(
                    "meshName" to snapshot.meshName,
                    "ready" to snapshot.isReady,
                    "readyPeerCount" to snapshot.readyPeerCount,
                    "peers" to snapshot.peers.map {
                        mapOf(
                            "nodeRid" to it.nodeRid.toString(),
                            "state" to it.state.name.lowercase(),
                        )
                    },
                ),
            )
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
        server.createContext("/profile/request") { exchange ->
            val request = exchange.readJson<ProfileReq>()
            exchange.writeJson(requestProfile(Contracts.PROFILE_CHANNEL, request, Duration.ofSeconds(5)))
        }
        server.createContext("/profile/command") { exchange ->
            val command = exchange.readJson<ProfileMsg>()
            sendProfile(Contracts.PROFILE_CHANNEL, command)
            exchange.writeJson(mapOf("status" to "sent"))
        }
        server.createContext("/profile/route/request") { exchange ->
            val request = exchange.readJson<ScenarioRoutePingReq>()
            exchange.writeJson(requestRoute(RoutingId.from("api-b"), request))
        }
        server.createContext("/profile/route/missing") { exchange ->
            val request = exchange.readJson<ScenarioRoutePingReq>()
            exchange.writeJson(
                routes.requestToNode(Contracts.PROFILE_ROUTE_CHANNEL, RoutingId.from("missing-rid"), request)
                    .timeout(Duration.ofMillis(300))
                    .submit(ScenarioRoutePingRes::class.java)
                    .handle { _, error -> RouteMissingRes(error != null) },
            )
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

    private fun requestProfile(channelName: String, request: ProfileReq, timeout: Duration): CompletionStage<ProfileRes> =
        requestProfile(channelName, request, timeout, System.nanoTime() + Duration.ofSeconds(30).toNanos())

    private fun requestProfile(
        channelName: String,
        request: ProfileReq,
        timeout: Duration,
        deadline: Long,
    ): CompletionStage<ProfileRes> = channels.requestToChannel(channelName, request)
        .timeout(timeout)
        .submit(ProfileRes::class.java)
        .handle { reply, error -> retryOrComplete(reply, error, deadline) {
            requestProfile(channelName, request, timeout, deadline)
        } }
        .thenCompose { it }

    private fun sendProfile(channelName: String, command: ProfileMsg) {
        channels.sendToChannel(channelName, command).submit()
    }

    private fun requestRoute(target: RoutingId, request: ScenarioRoutePingReq): CompletionStage<ScenarioRoutePingRes> =
        requestRoute(target, request, System.nanoTime() + Duration.ofSeconds(30).toNanos())

    private fun requestRoute(
        target: RoutingId,
        request: ScenarioRoutePingReq,
        deadline: Long,
    ): CompletionStage<ScenarioRoutePingRes> = routes.requestToNode(Contracts.PROFILE_ROUTE_CHANNEL, target, request)
        .timeout(Duration.ofSeconds(5))
        .submit(ScenarioRoutePingRes::class.java)
        .handle { reply, error -> retryOrComplete(reply, error, deadline) {
            requestRoute(target, request, deadline)
        } }
        .thenCompose { it }

    private fun <T> retryOrComplete(
        value: T?,
        error: Throwable?,
        deadline: Long,
        next: () -> CompletionStage<T>,
    ): CompletionStage<T> {
        if (error == null) return CompletableFuture.completedFuture(value!!)
        if (System.nanoTime() >= deadline) return CompletableFuture.failedFuture(error)
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
            else writeError(unwrap(error))
        }
    }

    private fun HttpExchange.writeError(error: Throwable) {
        val bytes = mapper.writeValueAsBytes(
            mapOf("error" to (error.message ?: error.javaClass.name)),
        )
        responseHeaders.add("content-type", "application/json")
        sendResponseHeaders(500, bytes.size.toLong())
        responseBody.use { it.write(bytes) }
    }

    private fun unwrap(error: Throwable): Throwable =
        if (error is CompletionException && error.cause != null) error.cause!! else error
}
