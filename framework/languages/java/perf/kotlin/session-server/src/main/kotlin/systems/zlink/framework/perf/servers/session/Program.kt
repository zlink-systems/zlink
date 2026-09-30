package systems.zlink.framework.perf.servers.session

import systems.zlink.framework.perf.ServerApplication

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.role() == "session" && !config.source()) { "Kotlin session-server requires a receiver session role." }
    when (config.scenario()) {
        "session-echo-only" -> SessionEchoOnlyScenario.run(config)
        "cs-remote-session-actor-echo" -> CsRemoteSessionActorEchoScenario.run(config)
        else -> error("session-server does not run ${config.scenario()}.")
    }
}
