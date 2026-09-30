package systems.zlink.framework.perf.servers.spot

import java.time.Duration
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.await
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.spots.ZLinkSpotCreateState
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.perf.servers.spot.PerfSpot

internal const val SPOT_TYPE = "perf-spot"

internal class KotlinSpotHandlerMarker

internal object KotlinSpotRole {
    fun <TSpot : PerfSpot> application(
        config: RoleConfig,
        spotType: Class<TSpot>,
        callsChannel: Boolean,
        extraOptions: ((ZLinkFrameworkOptions) -> Unit)? = null,
    ): ServerApplication = ServerApplication.create(config).configure { options ->
        options.useCoroutineHandlers(Dispatchers.IO)
        extraOptions?.invoke(options)
        val mesh = ServerApplication.routeMesh(options, config, "perf-spot")
        if (callsChannel) mesh.channelName(config.channelName()).client()
        mesh.objects().server().addSpotFactory(SPOT_TYPE, spotType) { builder ->
            builder.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
            builder.stableTypeLimit(config.spotIds().size)
            builder.disableRelocation()
        }
    }.bean(ObjectsReadiness::class.java) {
        ObjectsReadiness(false, "This cell has not created its User Spots yet.")
    }

    suspend fun createSpots(
        config: RoleConfig,
        manager: ZLinkSpotManager,
        mesh: ZLinkRouteMeshRuntime,
    ): Map<String, Any> {
        val created = config.spotIds().map { spotId ->
            val result = manager.kotlin().getOrCreate(spotId, SPOT_TYPE)
                .timeout(Duration.ofMillis(config.workload().setupTimeoutMs().toLong()))
                .await()
            check(result.state() != ZLinkSpotCreateState.REJECTED) { "Spot $spotId was rejected." }
            mapOf("spotId" to spotId, "state" to result.state().toString(), "meshName" to result.spot().meshName())
        }
        val placement = mesh.snapshot(config.meshName()).placement()
        check(placement.isAvailable() && placement.activeSpotCount() >= config.spotIds().size) {
            "The public mesh placement snapshot does not report every prepared Spot."
        }
        return Evidence.of("spotCreate", "Kotlin ZLinkSpotManager.getOrCreate(...).await()", mapOf(
            "spots" to created,
            "isAvailable" to placement.isAvailable(),
            "activeSpotCount" to placement.activeSpotCount(),
            "expectedSpots" to config.spotIds().size,
        ))
    }
}
