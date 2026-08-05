package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi

import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology

fun main(args: Array<String>) {
    CommerceApiApplication.run(SampleTopology.configPath(args))
}
