package MyGame

import java.io.File
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Flow
import java.util.concurrent.TimeUnit
import org.springframework.boot.WebApplicationType
import org.springframework.boot.builder.SpringApplicationBuilder
import systems.zlink.framework.configuration.*
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.monitoring.*
import systems.zlink.framework.runtime.host.*
import systems.zlink.framework.spots.*

private fun createZone(spots: ZLinkSpotManager, mesh: String, id: String) {
    val result =
        spots.getOrCreate(id, Names.ZONE).inMesh(mesh).submit().toCompletableFuture().join()
    check(result.state() != ZLinkSpotCreateState.REJECTED) { "Zone creation rejected: $id" }
}

fun configure(
    options: ZLinkFrameworkOptions,
    locations: ZLinkRedisLocationStore,
    relocations: ZLinkRedisRelocationStore,
) {
    options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
    options.configureLocations().setMessageFollowDuration(Control.MESSAGE_FOLLOW)
    options.addLocationStore(locations)
    options.addRelocationStore(relocations)
}

fun <T> observe(
    publisher: Flow.Publisher<T>,
    ready: CompletableFuture<Void>,
    action: (T) -> Unit,
): Flow.Subscription {
    val subscribed = CompletableFuture<Flow.Subscription>()
    publisher.subscribe(
        object : Flow.Subscriber<T> {
            override fun onSubscribe(subscription: Flow.Subscription) {
                subscribed.complete(subscription)
                subscription.request(Long.MAX_VALUE)
            }

            override fun onNext(item: T) = action(item)

            override fun onError(error: Throwable) {
                ready.completeExceptionally(error)
                error.printStackTrace(System.err)
            }

            override fun onComplete() {
                ready.completeExceptionally(
                    IllegalStateException("Runtime observation ended before Serving")
                )
            }
        }
    )
    return subscribed.join()
}

fun runNode(application: Class<*>, args: Array<String>) {
    require(args.size == 1) { "Supply a node configuration path" }
    System.setProperty("MyGame.config", File(args.single()).absolutePath)
    SpringApplicationBuilder(application, Stores::class.java)
        .web(WebApplicationType.NONE)
        .properties("spring.main.banner-mode=off")
        .run()
        .use { app ->
            val config = app.getBean(NodeConfig::class.java)
            val runtime = app.getBean(ZLinkFrameworkRuntime::class.java)
            val ready = CompletableFuture<Void>()
            val safe = CompletableFuture<Void>()
            val subscription =
                observe(runtime.observe(), ready) { observation ->
                    val status = observation.status()
                    if (runtime.messageFlowMode() != ZLinkMessageFlowLogMode.OFF) {
                        System.err.println(
                            "RUNTIME state=${status.state()} ready=${status.isReady()} safeToShutdown=${status.safeToShutdown()} relocation=${status.relocationResult()} termination=${status.terminationResult()}"
                        )
                    }
                    if (observation.loss().discardedTerminalCount() != 0L)
                        ready.completeExceptionally(IllegalStateException("Startup observation lost"))
                    else if (status.state() == ZLinkFrameworkRuntimeState.SERVING)
                        ready.complete(null)
                    else if (status.state() != ZLinkFrameworkRuntimeState.PREPARING)
                        ready.completeExceptionally(
                            IllegalStateException("Runtime startup failed: ${status.state()}")
                        )
                    if (
                        status.state() == ZLinkFrameworkRuntimeState.RELOCATED &&
                            status.safeToShutdown()
                    )
                        safe.complete(null)
                }
            try {
                ready.get(Control.HOST_DEADLINE.toSeconds(), TimeUnit.SECONDS)
                val mesh = Names.mesh(requireNotNull(config.worldId))
                if (config.zoneIds.isNotEmpty()) {
                    val spots = app.getBean(ZLinkSpotManager::class.java)
                    for (id in config.zoneIds) {
                        createZone(spots, mesh, id)
                    }
                }
                val endpoints =
                    linkedMapOf(
                        "id" to config.id,
                        "meshEndpoint" to
                            runtime.listenerStatus(ZLinkListenerKind.ROUTE_MESH, mesh).endpoint(),
                        "streamEndpoint" to
                            config.streamEndpoint?.let {
                                runtime
                                    .listenerStatus(ZLinkListenerKind.STREAM, Names.STREAM)
                                    .endpoint()
                            },
                    )
                println(Control.READY + Json.mapper.writeValueAsString(endpoints))
                val input = System.`in`.bufferedReader()
                while (true) {
                    when (val command = input.readLine() ?: break) {
                        Control.DRAIN -> {
                            println(Control.RELOCATION_REQUESTED)
                            val result =
                                runtime
                                    .relocate(
                                        ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            Control.HOST_DEADLINE,
                                        )
                                    )
                                    .toCompletableFuture()
                                    .join()
                            check(result.outcome() == ZLinkFrameworkRelocationOutcome.RELOCATED) {
                                result.toString()
                            }
                            println(Control.RELOCATED)
                            safe.get(Control.SAFE_DEADLINE.toSeconds(), TimeUnit.SECONDS)
                            println(Control.SAFE_TO_SHUTDOWN)
                        }
                        Control.CREATE_UPDATE_ZONE -> {
                            createZone(
                                app.getBean(ZLinkSpotManager::class.java),
                                mesh,
                                Names.zone(Names.UPDATE_CHANNEL),
                            )
                            println(Control.UPDATE_ZONE_CREATED)
                        }
                        Control.SHUTDOWN -> {
                            runtime.shutdown().toCompletableFuture().join()
                            check(runtime.status().state() == ZLinkFrameworkRuntimeState.STOPPED) {
                                runtime.status().toString()
                            }
                            println(Control.STOPPED)
                            return
                        }
                        else -> error("Unknown control command: $command")
                    }
                }
            } finally {
                subscription.cancel()
            }
        }
}
