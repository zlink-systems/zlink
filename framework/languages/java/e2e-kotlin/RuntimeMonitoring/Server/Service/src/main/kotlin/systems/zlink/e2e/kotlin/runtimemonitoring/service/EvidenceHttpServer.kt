package systems.zlink.e2e.kotlin.runtimemonitoring.service

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter
import systems.zlink.framework.locations.ZLinkPageRequest
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import org.springframework.beans.factory.ObjectProvider
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.util.concurrent.Flow

class EvidenceHttpServer @JvmOverloads constructor(
    private val state: EvidenceState,
    private val json: ObjectMapper,
    private val endpoint: String?,
    private val runtimeOptions: ZLinkChannelRuntimeOptions? = null,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>? = null,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (endpoint.isNullOrBlank()) {
            return
        }
        try {
            val uri = URI.create(endpoint)
            val nextServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            nextServer.createContext("/health") { exchange -> write(exchange, "ok\n") }
            nextServer.createContext("/evidence") { exchange ->
                write(exchange, json.writeValueAsString(state.snapshot()))
            }
            nextServer.createContext("/runtime/snapshot") { exchange ->
                val current = runtime?.ifAvailable
                    ?: throw IllegalStateException("runtime monitoring is not configured")
                write(exchange, json.writeValueAsString(snapshot(current)))
            }
            nextServer.createContext("/runtime/topology") { exchange ->
                val current = runtime?.ifAvailable
                    ?: throw IllegalStateException("runtime monitoring is not configured")
                val page = current.monitoringLocationRuntimeQuery()
                    .listTopology(ZLinkLocationTopologyFilter.all(), ZLinkPageRequest(1000, null))
                    .toCompletableFuture()
                    .join()
                write(exchange, json.writeValueAsString(page.items().map { row ->
                    mapOf(
                        "meshName" to row.meshName(),
                        "nodeRid" to row.nodeRid().toString(),
                        "state" to row.state().name,
                        "draining" to row.draining(),
                    )
                }))
            }
            nextServer.createContext("/runtime/channel-snapshot") { exchange ->
                val current = runtime?.ifAvailable
                    ?: throw IllegalStateException("runtime monitoring is not configured")
                val status = current.clientServerRuntime().snapshot(Contracts.CHANNEL)
                write(exchange, json.writeValueAsString(mapOf(
                    "ready" to status.isReady,
                    "readyTargetCount" to status.readyTargetCount(),
                    "sequence" to status.sequence(),
                    "targets" to status.targets().map { mapOf("nodeRid" to it.nodeRid().toString(), "state" to it.state().name) },
                )))
            }
            nextServer.createContext("/runtime/observer/start") { exchange ->
                startObserver()
                write(exchange, json.writeValueAsString(state.observerStatus()))
            }
            nextServer.createContext("/runtime/observer/status") { exchange ->
                write(exchange, json.writeValueAsString(state.observerStatus()))
            }
            nextServer.createContext("/runtime/unknown-mesh") { exchange ->
                invalidMeshQuery(exchange, observe = false)
            }
            nextServer.createContext("/runtime/unknown-observe") { exchange ->
                invalidMeshQuery(exchange, observe = true)
            }
            nextServer.createContext("/runtime/placement/spot/create") { exchange ->
                createSpot(exchange)
            }
            nextServer.createContext("/runtime/placement/spot/close") { exchange ->
                closeSpot(exchange)
            }
            nextServer.createContext("/runtime/placement/actor/create") { exchange ->
                createActor(exchange)
            }
            nextServer.createContext("/runtime/placement/actor/destroy") { exchange ->
                destroyActor(exchange)
            }
            nextServer.createContext("/shutdown") { exchange ->
                if (exchange.requestMethod != "POST") {
                    exchange.sendResponseHeaders(405, -1)
                    exchange.close()
                    return@createContext
                }
                write(exchange, "{\"status\":\"shutting-down\"}\n")
                Thread({ System.exit(0) }, "runtime-monitoring-shutdown").apply {
                    isDaemon = false
                    start()
                }
            }
            nextServer.createContext("/crash") { exchange ->
                write(exchange, "{\"status\":\"crashing\"}\n")
                Thread({ Runtime.getRuntime().halt(137) }, "runtime-monitoring-crash").apply {
                    isDaemon = false
                    start()
                }
            }
            if (runtimeOptions != null) {
                nextServer.createContext("/admin/drain") { exchange -> setWeight(exchange, 0, "drained") }
                nextServer.createContext("/admin/restore") { exchange -> setWeight(exchange, 100, "restored") }
            }
            nextServer.start()
            server = nextServer
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start evidence endpoint $endpoint", error)
        }
    }

    private fun setWeight(
        exchange: HttpExchange,
        weight: Int,
        status: String,
    ) {
        if (exchange.requestMethod != "POST") {
            exchange.sendResponseHeaders(405, -1)
            exchange.close()
            return
        }
        runtimeOptions
            ?.clientServerChannel(Contracts.CHANNEL)
            ?.configureServerSocket()
            ?.weight(weight)
        state.record("admin", Contracts.CHANNEL, "WeightChanged", "status=$status|weight=$weight")
        write(exchange, "{\"status\":\"$status\",\"weight\":$weight}\n")
    }

    private fun snapshot(current: ZLinkFrameworkRuntime): Contracts.RuntimeSnapshot {
        val status = current.routeMeshRuntime().snapshot(Contracts.SPOT_MESH)
        return Contracts.RuntimeSnapshot(
            status.meshName(),
            status.state().name,
            status.isReady,
            status.readyPeerCount(),
            status.sequence(),
            status.peers().map { peer ->
                Contracts.RuntimePeer(
                    peer.nodeRid().toString(),
                    peer.state().name,
                    peer.unavailableReason().map { it.name }.orElse(""),
                )
            },
            status.channels().map { channel ->
                Contracts.RuntimeChannel(
                    channel.channelName(),
                    channel.isReady,
                    channel.readyTargetCount(),
                )
            },
            status.placement().isAvailable,
            status.placement().activeActorCount(),
            status.placement().activeSpotCount(),
            status.placement().unavailableReason().map { it.name }.orElse(""),
        )
    }

    private fun startObserver() {
        val current = runtime?.ifAvailable
            ?: throw IllegalStateException("runtime monitoring is not configured")
        if (state.observerStatus().started) return
        state.observerStatus(Contracts.ObserverStatus(true, 0, false, 0, ""))
        current.routeMeshRuntime().observe(Contracts.SPOT_MESH, 32).subscribe(
            object : Flow.Subscriber<systems.zlink.framework.monitoring.ZLinkObservedStatus<systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot>> {
                override fun onSubscribe(subscription: Flow.Subscription) {
                    subscription.request(Long.MAX_VALUE)
                }

                override fun onNext(item: systems.zlink.framework.monitoring.ZLinkObservedStatus<systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot>) {
                    val status = item.status()
                    state.observerStatus(
                        Contracts.ObserverStatus(true, status.sequence(), status.isReady, status.readyPeerCount(), ""),
                    )
                }

                override fun onError(throwable: Throwable) {
                    val old = state.observerStatus()
                    state.observerStatus(old.copy(error = throwable.javaClass.name + ": " + throwable.message))
                }

                override fun onComplete() = Unit
            },
        )
    }

    private fun invalidMeshQuery(exchange: HttpExchange, observe: Boolean) {
        try {
            val current = runtime?.ifAvailable
                ?: throw IllegalStateException("runtime monitoring is not configured")
            if (observe) {
                current.routeMeshRuntime().observe("missing-mesh", 2)
            } else {
                current.routeMeshRuntime().snapshot("missing-mesh")
            }
            write(exchange, 500, "{\"accepted\":true}\n")
        } catch (error: Throwable) {
            write(exchange, 400, "{\"error\":\"${error.javaClass.simpleName}\"}\n")
        }
    }

    private fun createSpot(exchange: HttpExchange) {
        try {
            val id = query(exchange, "id")
            val current = runtime?.ifAvailable ?: error("runtime monitoring is not configured")
            val result = current.spotManager().getOrCreate(id, "monitoring")
                .inMesh(Contracts.SPOT_MESH)
                .request(ZLinkMessage.of("placement-spot"))
                .submit().toCompletableFuture().join()
            write(exchange, 200, json.writeValueAsString(mapOf("accepted" to true, "state" to result.state().name)))
        } catch (error: Throwable) {
            write(exchange, 409, "{\"accepted\":false,\"error\":\"${errorMessage(error)}\"}\n")
        }
    }

    private fun closeSpot(exchange: HttpExchange) {
        try {
            val id = query(exchange, "id")
            val current = runtime?.ifAvailable ?: error("runtime monitoring is not configured")
            val found = current.spotManager().find(id).toCompletableFuture().join()
            val closed = found.isPresent && current.spotManager().close(found.get()).toCompletableFuture().join()
            write(exchange, 200, json.writeValueAsString(mapOf("accepted" to closed, "closed" to closed)))
        } catch (error: Throwable) {
            write(exchange, 409, "{\"accepted\":false,\"error\":\"${errorMessage(error)}\"}\n")
        }
    }

    private fun createActor(exchange: HttpExchange) {
        try {
            val id = query(exchange, "id")
            val current = runtime?.ifAvailable ?: error("runtime monitoring is not configured")
            val result = current.actorManager().getOrCreate(id, Contracts.ACTOR_TYPE)
                .inMesh(Contracts.SPOT_MESH)
                .request(Contracts.WorkReq("placement-actor"))
                .submit().toCompletableFuture().join()
            write(exchange, 200, json.writeValueAsString(mapOf("accepted" to true, "state" to result.javaClass.simpleName)))
        } catch (error: Throwable) {
            write(exchange, 409, "{\"accepted\":false,\"error\":\"${errorMessage(error)}\"}\n")
        }
    }

    private fun destroyActor(exchange: HttpExchange) {
        try {
            val id = query(exchange, "id")
            val current = runtime?.ifAvailable ?: error("runtime monitoring is not configured")
            val found = current.actorManager().find(id).toCompletableFuture().join()
            val destroyed = found.isPresent && current.actorManager().destroy(found.get()).toCompletableFuture().join()
            write(exchange, 200, json.writeValueAsString(mapOf("destroyed" to destroyed)))
        } catch (error: Throwable) {
            write(exchange, 409, "{\"destroyed\":false,\"error\":\"${errorMessage(error)}\"}\n")
        }
    }

    private fun query(exchange: HttpExchange, name: String): String {
        val value = exchange.requestURI.rawQuery?.split("&")
            ?.firstOrNull { it.startsWith("$name=") }
            ?.substringAfter('=')
        return value?.let { java.net.URLDecoder.decode(it, StandardCharsets.UTF_8) }
            ?: error("missing query parameter $name")
    }

    private fun errorMessage(error: Throwable): String =
        (error.cause ?: error).javaClass.simpleName

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun write(
        exchange: HttpExchange,
        value: String,
    ) {
        write(exchange, 200, value)
    }

    private fun write(
        exchange: HttpExchange,
        status: Int,
        value: String,
    ) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }
}
