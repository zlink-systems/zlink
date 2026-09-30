package systems.zlink.framework.perf.servers.publisher

import systems.zlink.framework.perf.ServerApplication

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.scenario() == "pubsub-fanout-echo" && config.role() == "publisher" && config.source()) {
        "publisher-server runs the publisher role of pubsub-fanout-echo."
    }
    PubSubFanoutEchoScenario.run(config, ServerApplication.cellDirectory(args))
}
