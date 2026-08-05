package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology

fun main(args: Array<String>) {
    OrderWorkflowApplication.run(SampleTopology.configPath(args))
}
