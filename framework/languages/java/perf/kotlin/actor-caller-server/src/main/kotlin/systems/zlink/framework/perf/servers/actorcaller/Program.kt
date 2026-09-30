package systems.zlink.framework.perf.servers.actorcaller

import systems.zlink.framework.perf.ServerApplication

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.role() == "actor-caller" && config.source()) {
        "actor-caller-server runs the source role of §10.9 and §10.10."
    }
    when (config.scenario()) {
        "actor-no-bind-request-echo" -> ActorNoBindRequestEchoScenario.run(config)
        "actor-no-bind-send-send-echo" -> ActorNoBindSendSendEchoScenario.run(config)
        else -> error("Unsupported Kotlin ActorCaller scenario: ${config.scenario()}")
    }
}
