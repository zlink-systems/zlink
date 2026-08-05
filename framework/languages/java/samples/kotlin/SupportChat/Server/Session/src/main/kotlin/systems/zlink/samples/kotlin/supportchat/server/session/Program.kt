package systems.zlink.samples.kotlin.supportchat.server.session

import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTopology

fun main(args: Array<String>) {
    val app = SessionApplication.run(SampleTopology.configPath(args))
    Runtime.getRuntime().addShutdownHook(Thread { app.close() })
    Thread.currentThread().join()
}
