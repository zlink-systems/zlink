package systems.zlink.framework.perf.servers.spot

import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.perf.kotlin.completionStage

internal object S2sChannelToSpotEchoTarget {
    fun run(config: RoleConfig) {
        require(config.role() == "spot" && !config.source())
        val sendSend = config.scenario() == "s2s-channel-to-spot-send-send-echo"
        if (sendSend) start(config, KotlinChannelToSpotSendSpot::class.java, true)
        else start(config, KotlinPerfEchoSpot::class.java, false)
    }

    private fun <TSpot : systems.zlink.framework.perf.servers.spot.PerfSpot> start(
        config: RoleConfig,
        spotType: Class<TSpot>,
        callsChannel: Boolean,
    ) {
        val app = KotlinSpotRole.application(config, spotType, callsChannel)
        app.bean(ScenarioMetrics::class.java) { ScenarioMetrics(app.measurement()).spotInternalsUnsupported() }
        val context = app.start()
        completionStage {
            val evidence = KotlinSpotRole.createSpots(
                config,
                context.getBean(ZLinkSpotManager::class.java),
                context.getBean(ZLinkRouteMeshRuntime::class.java),
            )
            context.getBean(ObjectsReadiness::class.java).set(true, "", listOf(evidence))
        }.exceptionally { error -> app.measurement().recordDiagnostic(error); null }
    }
}
