package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode

import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

fun main(args: Array<String>) {
    SampleTopology.configure(args)
    CourierSpotNodeApplication.run()
}
