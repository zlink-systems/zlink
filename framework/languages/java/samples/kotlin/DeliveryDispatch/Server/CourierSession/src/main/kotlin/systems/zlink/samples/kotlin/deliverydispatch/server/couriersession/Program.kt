package systems.zlink.samples.kotlin.deliverydispatch.server.couriersession

import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

fun main(args: Array<String>) {
    SampleTopology.configure(args)
    CourierSessionApplication.run()
}
