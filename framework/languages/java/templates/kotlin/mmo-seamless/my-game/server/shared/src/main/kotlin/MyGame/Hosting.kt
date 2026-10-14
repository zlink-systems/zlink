package MyGame

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.io.File
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Flow
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import org.springframework.boot.WebApplicationType
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.locations.redis.*
import systems.zlink.framework.monitoring.*
import systems.zlink.framework.runtime.host.*
import systems.zlink.framework.spots.*

data class NodeConfig(
    val id: String = "",
    val redisEndpoint: String = "",
    val keyPrefix: String = "",
    val meshEndpoint: String = "tcp://127.0.0.1:0",
    val streamEndpoint: String? = null,
    val zoneIds: List<String> = emptyList(),
    val zoneCapacity: Int = 0,
    val bootstrap: Boolean = false,
)

object Json {
    val mapper = jacksonObjectMapper().findAndRegisterModules()
}

@Configuration(proxyBeanMethods = false)
class Stores {
    @Bean
    fun config(): NodeConfig =
        Json.mapper.readValue(File(System.getProperty("MyGame.config")), NodeConfig::class.java)

    @Bean(destroyMethod = "close")
    fun locations(config: NodeConfig) =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(config.redisEndpoint)
                .setKeyPrefix(config.keyPrefix + "locations:")
        )

    @Bean(destroyMethod = "close")
    fun relocations(config: NodeConfig) =
        ZLinkRedisRelocationStore(
            ZLinkRedisRelocationOptions()
                .setConnectionString(config.redisEndpoint)
                .setKeyPrefix(config.keyPrefix + "relocation:")
        )
}

fun configure(
    options: ZLinkFrameworkOptions,
    locations: ZLinkRedisLocationStore,
    relocations: ZLinkRedisRelocationStore,
) {
    options.useCoroutineHandlers(Dispatchers.Default)
    options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
    options.configureLocations()
    options.addLocationStore(locations)
    options.addRelocationStore(relocations)
}

fun <T> observe(publisher: Flow.Publisher<T>, action: (T) -> Unit): Flow.Subscription {
    val subscribed = CompletableFuture<Flow.Subscription>()
    publisher.subscribe(
        object : Flow.Subscriber<T> {
            override fun onSubscribe(subscription: Flow.Subscription) {
                subscribed.complete(subscription)
                subscription.request(Long.MAX_VALUE)
            }

            override fun onNext(item: T) = action(item)

            override fun onError(error: Throwable) {
                error.printStackTrace(System.err)
            }

            override fun onComplete() {}
        }
    )
    return subscribed.join()
}

fun runNode(application: Class<*>, args: Array<String>) = runBlocking {
    require(args.size == 1) { "Supply a node configuration path" }
    System.setProperty("MyGame.config", File(args[0]).absolutePath)
    val app =
        SpringApplicationBuilder(application, Stores::class.java)
            .web(WebApplicationType.NONE)
            .properties(
                "spring.main.banner-mode=off",
                "logging.pattern.console=%msg%n",
                "logging.file.name=",
            )
            .run()
    try {
        val config = app.getBean(NodeConfig::class.java)
        val runtime = app.getBean(ZLinkFrameworkRuntime::class.java)
        val ready = CompletableFuture<Void>()
        val safe = CompletableFuture<Void>()
        val statusSubscription =
            observe(runtime.observe()) { observed ->
                val status = observed.status()
                if (runtime.messageFlowMode() != ZLinkMessageFlowLogMode.OFF) {
                    System.err.println(
                        "RUNTIME state=${status.state()} ready=${status.isReady} " +
                            "safeToShutdown=${status.safeToShutdown()} " +
                            "relocation=${status.relocationResult()} " +
                            "termination=${status.terminationResult()}"
                    )
                }
                if (status.isReady) ready.complete(null)
                if (
                    status.state() == ZLinkFrameworkRuntimeState.RELOCATED &&
                        status.safeToShutdown()
                )
                    safe.complete(null)
            }
        ready.get(Control.HOST_DEADLINE.toSeconds(), java.util.concurrent.TimeUnit.SECONDS)
        if (config.bootstrap) {
            val spots = app.getBean(ZLinkSpotManager::class.java)
            for (zoneId in config.zoneIds) {
                val result =
                    spots.kotlin().getOrCreate(zoneId, Names.ZONE).inMesh(Names.MESH).await()
                check(result.state() != ZLinkSpotCreateState.REJECTED) {
                    "Zone bootstrap rejected: $zoneId $result"
                }
            }
        }
        println(
            Control.READY +
                Json.mapper.writeValueAsString(
                    mapOf(
                        "id" to config.id,
                        "meshEndpoint" to
                            runtime
                                .listenerStatus(ZLinkListenerKind.ROUTE_MESH, Names.MESH)
                                .endpoint(),
                        "streamEndpoint" to
                            config.streamEndpoint?.let {
                                runtime
                                    .listenerStatus(ZLinkListenerKind.STREAM, Names.STREAM)
                                    .endpoint()
                            },
                    )
                )
        )
        System.`in`.bufferedReader().useLines { commands ->
            for (command in commands) {
                when (command) {
                    Control.DRAIN -> {
                        println(Control.RELOCATION_REQUESTED)
                        val result = runBlocking {
                            runtime
                                .relocate(
                                    ZLinkFrameworkRelocationOptions(
                                        ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                        null,
                                        Control.HOST_DEADLINE,
                                    )
                                )
                                .await()
                        }
                        check(result.outcome() == ZLinkFrameworkRelocationOutcome.RELOCATED) {
                            result.toString()
                        }
                        println(Control.RELOCATED)
                        safe.get(
                            Control.HOST_DEADLINE.toSeconds(),
                            java.util.concurrent.TimeUnit.SECONDS,
                        )
                        println(Control.SAFE_TO_SHUTDOWN)
                    }
                    Control.SHUTDOWN -> {
                        runtime.shutdown().toCompletableFuture().join()
                        check(runtime.status().state() == ZLinkFrameworkRuntimeState.STOPPED) {
                            runtime.status().toString()
                        }
                        println(Control.STOPPED)
                        break
                    }
                    else -> error("Unknown control command: $command")
                }
            }
        }
        statusSubscription.cancel()
    } finally {
        app.close()
    }
}
