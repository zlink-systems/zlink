package systems.zlink.samples.kotlin.shoppingmall.client

import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology

suspend fun main(args: Array<String>) {
    try {
        ClientApplication.run(SampleTopology.configPath(args))
        println("shoppingmall=completed")
        System.out.flush()
        System.err.flush()
        Runtime.getRuntime().halt(0)
    } catch (error: Throwable) {
        error.printStackTrace()
        System.err.flush()
        Runtime.getRuntime().halt(1)
    }
}
