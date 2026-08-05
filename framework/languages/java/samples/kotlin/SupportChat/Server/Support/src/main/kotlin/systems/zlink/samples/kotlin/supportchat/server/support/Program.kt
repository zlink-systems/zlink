package systems.zlink.samples.kotlin.supportchat.server.support

import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTopology

fun main(args: Array<String>) {
    val app = SupportApplication.run(SampleTopology.configPath(args))
    Runtime.getRuntime().addShutdownHook(Thread { app.close() })
    Thread.currentThread().join()
}
