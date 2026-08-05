package systems.zlink.e2e.kotlin.spotactortransfer.actor

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Duration
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.spotactortransfer.shared.Contracts
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotManager

class ActorNodeHttpServer(
    private val endpoint: String,
    private val json: ObjectMapper,
    private val evidence: EvidenceStore,
    private val gates: GateStore,
    private val spots: ZLinkSpotManager,
    private val actors: ZLinkActorManager,
    private val actorClient: ZLinkActorClient,
    private val requiredPeerCount: Int,
    private val lifecycle: systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var executor: java.util.concurrent.ExecutorService? = null
    private var running = false

    override fun start() {
        val uri = URI.create(endpoint)
        server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0).also { http ->
            executor = Executors.newCachedThreadPool()
            http.executor = executor
            http.createContext("/health") { exchange -> handle(exchange) {
                val mesh = lifecycle.routeMeshRuntime().snapshot(Contracts.MESH)
                if (!lifecycle.isReady()
                    || !mesh.isReady()
                    || mesh.readyPeerCount() < requiredPeerCount) {
                    writeText(exchange, 503, "Framework runtime is not ready")
                    return@handle
                }
                writeJson(exchange, mapOf("status" to "ready", "nodeRid" to evidence.nodeRid))
            } }
            http.createContext("/evidence") { exchange -> handle(exchange) {
                writeJson(exchange, evidence.snapshot())
            } }
            http.createContext("/spots") { exchange -> handle(exchange) { createSpot(exchange) } }
            http.createContext("/gates") { exchange -> handle(exchange) { releaseGate(exchange) } }
            http.createContext("/actors") { exchange -> handle(exchange) { actorOperation(exchange) } }
            http.start()
        }
        running = true
    }

    private fun createSpot(exchange: HttpExchange) {
        val request = json.readValue(exchange.requestBody, Contracts.CreateSpotReq::class.java)
        val result = spots.getOrCreate(
            request.spotRid(),
            TransferUserSpot::class.java.name,
        ).request(ZLinkMessage.of(request))
            .timeout(Duration.ofSeconds(5))
            .submit()
            .toCompletableFuture().get(12, TimeUnit.SECONDS)
        writeJson(exchange, Contracts.CreateSpotRes(
            result.spot().spotId(), result.spot().nodeRid().toString(), result.state().name,
        ))
    }

    private fun releaseGate(exchange: HttpExchange) {
        val path = pathParts(exchange)
        require(path.size == 2) { "gate path must be /gates/{key}" }
        writeJson(exchange, Contracts.GateReleaseRes(path[1], gates.release(path[1])))
    }

    private fun actorOperation(exchange: HttpExchange) {
        val path = pathParts(exchange)
        if (path.size == 1) {
            createActor(exchange)
            return
        }
        require(path.size == 3) { "actor path must identify an operation" }
        when (path[2]) {
            "join" -> joinActor(exchange, path[1])
            "probe" -> probeActor(exchange, path[1])
            "probe-timeout" -> probeActorWithTimeout(exchange, path[1])
            "probe-ref" -> probeActorRef(exchange, path[1])
            "send-ref" -> sendActorRef(exchange, path[1])
            "ref" -> actorRef(exchange, path[1])
            else -> error("unknown actor operation: ${path[2]}")
        }
    }

    private fun createActor(exchange: HttpExchange) {
        val request = json.readValue(exchange.requestBody, Contracts.ActorCreateReq::class.java)
        val result = actors.getOrCreate(request.actorId(), request.actorType())
            .request(request)
            .submit()
            .toCompletableFuture().get(12, TimeUnit.SECONDS)
        val actor = when (result) {
            is ZLinkActorCreateResult.Created -> result.actor()
            is ZLinkActorCreateResult.Existing -> result.actor()
            is ZLinkActorCreateResult.Rejected ->
                throw IllegalStateException("Actor creation was rejected")
        }
        writeJson(exchange, Contracts.ActorCreateRes(
            actor.actorId(), request.actorType(), actor.nodeRid().toString(),
            actor.objectGeneration(),
        ))
    }

    private fun joinActor(exchange: HttpExchange, actorId: String) {
        val request = json.readValue(exchange.requestBody, Contracts.JoinTargetReq::class.java)
        try {
            val result = actorClient.requestToActor(actorId, request)
                .timeout(Duration.ofSeconds(12))
                .submit(Contracts.JoinTargetRes::class.java).toCompletableFuture().get(12, TimeUnit.SECONDS)
            evidence.add(request.scenario(), actorId,
                if (result.accepted()) "success_reply" else "reject_reply", request.targetSpotRid())
            writeJson(exchange, result)
        } catch (error: Exception) {
            evidence.add(request.scenario(), actorId, "join_failed", rootMessage(error))
            writeJson(exchange, Contracts.JoinTargetRes(
                request.scenario(), actorId, false, evidence.nodeRid, request.targetSpotRid(), 0,
            ))
        }
    }

    private fun probeActor(exchange: HttpExchange, actorId: String) {
        val request = json.readValue(exchange.requestBody, Contracts.ProbeReq::class.java)
        val result = actorClient.requestToActor(actorId, request)
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.ProbeRes::class.java).toCompletableFuture().get(10, TimeUnit.SECONDS)
        writeJson(exchange, result)
    }

    private fun probeActorWithTimeout(exchange: HttpExchange, actorId: String) {
        val request = json.readValue(exchange.requestBody, Contracts.ProbeWithTimeoutReq::class.java)
        try {
            val result = actorClient.requestToActor(
                actorId,
                Contracts.ProbeReq(request.scenario(), request.marker()),
            ).timeout(Duration.ofMillis(request.timeoutMillis()))
                .submit(Contracts.ProbeRes::class.java).toCompletableFuture()
                .get(request.timeoutMillis() + 1_000, TimeUnit.MILLISECONDS)
            writeJson(exchange, result)
        } catch (error: Exception) {
            evidence.add(request.scenario(), actorId, "request_timeout", errorKind(error))
            throw error
        }
    }

    private fun actorRef(exchange: HttpExchange, actorId: String) {
        val actor = requireActor(actorId)
        writeJson(exchange, mapOf(
            "actorId" to actor.actorId(),
            "nodeRid" to actor.nodeRid().toString(),
            "objectGeneration" to actor.objectGeneration(),
        ))
    }

    private fun probeActorRef(exchange: HttpExchange, actorId: String) {
        json.readValue(exchange.requestBody, Contracts.ProbeAtRefReq::class.java)
        throw UnsupportedOperationException(
            "ST-F4/F5 require a transport delivery-delay fixture; public Actor messaging "
                + "does not accept an owner route.",
        )
    }

    private fun sendActorRef(exchange: HttpExchange, actorId: String) {
        json.readValue(exchange.requestBody, Contracts.SendAtRefReq::class.java)
        throw UnsupportedOperationException(
            "ST-F4/F5 require a transport delivery-delay fixture; public Actor messaging "
                + "does not accept an owner route.",
        )
    }

    private fun requireActor(actorId: String) = actors.find(actorId)
        .toCompletableFuture().get(5, TimeUnit.SECONDS)
        .orElseThrow { IllegalStateException("actor was not found: $actorId") }

    private fun errorKind(error: Throwable): String {
        var current = error
        while ((current is java.util.concurrent.CompletionException
                || current is java.util.concurrent.ExecutionException)
            && current.cause != null) {
            current = current.cause!!
        }
        return if (current is ZLinkFrameworkException) current.kind().name else rootMessage(current)
    }

    private fun pathParts(exchange: HttpExchange): List<String> =
        exchange.requestURI.path.split('/').filter(String::isNotBlank)

    private fun handle(exchange: HttpExchange, action: () -> Unit) {
        try {
            action()
        } catch (error: Exception) {
            error.printStackTrace(System.err)
            System.err.println(rootMessage(error))
            writeText(exchange, 500, rootMessage(error))
        }
    }

    private fun writeJson(exchange: HttpExchange, value: Any) {
        val body = json.writeValueAsBytes(value)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(200, body.size.toLong())
        exchange.responseBody.use { it.write(body) }
    }

    private fun writeText(exchange: HttpExchange, status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.use { it.write(body) }
    }

    override fun stop() {
        server?.stop(0)
        server = null
        executor?.shutdownNow()
        executor = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun rootMessage(error: Throwable): String {
        val current = generateSequence(error) { it.cause }.last()
        return when (current) {
            is systems.zlink.contracts.errors.ZlinkRequestException ->
                "${current.javaClass.name}: result=${current.getResult()}, " +
                    "errno=${current.getNativeErrno()}"
            is systems.zlink.contracts.errors.ZlinkSubmitException ->
                "${current.javaClass.name}: result=${current.getResult()}, " +
                    "errno=${current.getNativeErrno()}"
            else -> current.toString()
        }
    }
}
