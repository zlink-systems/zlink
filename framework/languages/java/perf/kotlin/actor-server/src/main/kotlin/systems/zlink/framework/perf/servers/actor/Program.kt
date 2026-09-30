package systems.zlink.framework.perf.servers.actor

import kotlinx.coroutines.Dispatchers
import systems.zlink.framework.kotlin.addHandlersFromPackageOf
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.perf.ActorPlacementWatcher
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.kotlin.PerfKotlinActorMarker
import systems.zlink.framework.perf.kotlin.addPerfKotlinActors

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.role() == "actor" && config.scenario() in setOf(
        "cs-remote-session-actor-echo", "actor-no-bind-request-echo", "actor-no-bind-send-send-echo",
    )) { "actor-server runs the actor role of §10.2, §10.9 and §10.10." }
    val app = ServerApplication.create(config).configure { options ->
        options.addHandlersFromPackageOf<PerfKotlinActorMarker>()
        options.useCoroutineHandlers(Dispatchers.IO)
        val mesh = ServerApplication.routeMesh(options, config, "perf-actor")
        if (config.mode() == "send-send") mesh.channelName(config.channelName()).client()
        addPerfKotlinActors(mesh.objects().server())
    }.bean(ObjectsReadiness::class.java) {
        ObjectsReadiness(false, "No typed probe has reached a Kotlin Actor yet.")
    }.bean(ActorPlacementWatcher::class.java)
    app.start().getBean(ActorPlacementWatcher::class.java).start()
}
