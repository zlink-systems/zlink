package systems.zlink.framework.perf.servers.sessionactorlocal

import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.servers.session.KotlinPerfSessionActorRelayHandler
import systems.zlink.framework.perf.kotlin.PerfKotlinActorMarker
import systems.zlink.framework.perf.kotlin.addPerfKotlinActors
import systems.zlink.framework.kotlin.addHandlersFromPackageOf
import systems.zlink.framework.kotlin.useCoroutineHandlers
import kotlinx.coroutines.Dispatchers

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.role() == "session-actor-local" && config.scenario() == "cs-local-session-actor-echo") {
        "session-actor-local-server runs the cs-local-session-actor-echo role."
    }
    val app = ServerApplication.create(config).configure { options ->
        options.addHandlersFromPackageOf<PerfKotlinActorMarker>()
        options.useCoroutineHandlers(Dispatchers.IO)
        val mesh = ServerApplication.routeMesh(options, config, "perf-session-actor-local")
        addPerfKotlinActors(mesh.objects().server())
        options.addStreamNode("perf-session")
            .bind(config.transportEndpoints().get("stream"))
            .enableActorDispatch()
            .registerSession(KotlinPerfSessionActorRelayHandler::class.java)
    }
    app.bean(ObjectsReadiness::class.java) { ObjectsReadiness(false, "No Actor is bound to a session yet.") }
    app.start()
}
