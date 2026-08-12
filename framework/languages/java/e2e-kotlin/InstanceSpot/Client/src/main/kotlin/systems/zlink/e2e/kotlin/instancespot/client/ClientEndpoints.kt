package systems.zlink.e2e.kotlin.instancespot.client

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpServer
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import java.util.concurrent.ExecutionException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlinx.coroutines.runBlocking
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.instancespot.shared.Contracts
import systems.zlink.e2e.kotlin.instancespot.shared.HttpSupport
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.framework.kotlin.sendToSpot
import systems.zlink.framework.spots.ZLinkSpotManager

class ClientEndpoints(
    private val options: ClientOptions,
    private val routes: ZLinkRouteClient,
    private val spots: ZLinkSpotManager,
    private val json: ObjectMapper,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var executor: ExecutorService? = null

    override fun start() {
        val http = HttpSupport.server(options.httpEndpoint)
        val pool = Executors.newFixedThreadPool(8)
        executor = pool
        http.executor = pool
        http.createContext("/health") { exchange ->
            HttpSupport.write(exchange, json, mapOf("status" to "ready", "rid" to options.rid))
        }
        http.createContext("/lookup") { exchange ->
            val body = json.readTree(HttpSupport.body(exchange))
            HttpSupport.write(exchange, json, lookup(body.path("spotId").asText()))
        }
        http.createContext("/request") { exchange ->
            HttpSupport.write(exchange, json, request(HttpSupport.read(exchange, json, Contracts.InstanceReq::class.java)))
        }
        http.createContext("/send") { exchange ->
            HttpSupport.write(exchange, json, send(HttpSupport.read(exchange, json, Contracts.InstanceMsg::class.java)))
        }
        http.createContext("/close") { exchange ->
            HttpSupport.write(exchange, json, close(HttpSupport.read(exchange, json, Contracts.CloseMsg::class.java)))
        }
        http.createContext("/concurrent") { exchange ->
            HttpSupport.write(exchange, json, concurrent(HttpSupport.read(exchange, json, Contracts.ConcurrentReq::class.java)))
        }
        http.createContext("/shutdown") { exchange ->
            HttpSupport.write(exchange, json, mapOf("status" to "stopping"))
            HttpSupport.shutdownAsync()
        }
        http.start()
        server = http
    }

    private fun request(input: Contracts.InstanceReq): Contracts.InstanceCallRes = try {
        val reply = runBlocking {
            routes.kotlin()
                .requestToSpot<Contracts.InstanceRes>(input.spotId, input)
                .instanceSpot(Contracts.STABLE_TYPE)
                .inMesh(Contracts.MESH)
                .timeout(Duration.ofMillis(input.timeoutMilliseconds))
                .await()
        }
        Contracts.InstanceCallRes(true, reply, "", "")
    } catch (error: RuntimeException) {
        val cause = unwrap(error)
        Contracts.InstanceCallRes(false, null, errorKind(cause), cause.message.orEmpty())
    }

    private fun send(input: Contracts.InstanceMsg): Contracts.SendSubmitRes = try {
        runBlocking {
            routes.kotlin().sendToSpot(input.spotId, input)
                .instanceSpot(Contracts.STABLE_TYPE)
                .inMesh(Contracts.MESH)
                .await()
        }
        Contracts.SendSubmitRes(true, "", "")
    } catch (error: RuntimeException) {
        val cause = unwrap(error)
        Contracts.SendSubmitRes(false, errorKind(cause), cause.message.orEmpty())
    }

    private fun close(input: Contracts.CloseMsg): Contracts.SendSubmitRes = try {
        runBlocking {
            routes.kotlin().sendToSpot(input.spotId, input)
                .instanceSpot()
                .inMesh(Contracts.MESH)
                .await()
        }
        Contracts.SendSubmitRes(true, "", "")
    } catch (error: RuntimeException) {
        val cause = unwrap(error)
        Contracts.SendSubmitRes(false, errorKind(cause), cause.message.orEmpty())
    }

    private fun lookup(spotId: String): Contracts.LookupRes = try {
        val ref = runBlocking { spots.find(spotId).await() }.orElse(null)
        if (ref == null) Contracts.LookupRes(false, spotId, 0, "", "", "", "")
        else Contracts.LookupRes(
            true, ref.spotId(), ref.objectGeneration(), ref.meshName(), ref.nodeRid().toString(), "", "",
        )
    } catch (error: RuntimeException) {
        val cause = unwrap(error)
        Contracts.LookupRes(false, spotId, 0, "", "", errorKind(cause), cause.message.orEmpty())
    }

    private fun concurrent(input: Contracts.ConcurrentReq): Contracts.ConcurrentRes {
        require(input.count in 1..128) { "count must be between 1 and 128" }
        val tasks = (0 until input.count).map { index ->
            CompletableFuture.supplyAsync({
                request(
                    Contracts.InstanceReq(
                        input.spotId, "${input.operationPrefix}-$index", "payload-$index", input.timeoutMilliseconds,
                    ),
                )
        }, executor!!)
        }
        CompletableFuture.allOf(*tasks.toTypedArray()).join()
        return Contracts.ConcurrentRes(tasks.map(CompletableFuture<Contracts.InstanceCallRes>::join))
    }

    private fun errorKind(error: Throwable): String =
        (error as? ZLinkFrameworkException)?.kind()?.name ?: error.javaClass.simpleName

    private fun unwrap(error: Throwable): Throwable {
        var current = error
        while ((current is CompletionException || current is ExecutionException) && current.cause != null) {
            current = current.cause!!
        }
        return current
    }

    override fun stop() {
        server?.stop(0)
        server = null
        executor?.shutdownNow()
        executor = null
    }

    override fun isRunning(): Boolean = server != null
}
