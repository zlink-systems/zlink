package systems.zlink.samples.kotlin.deliverydispatch.server.tracking

import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

fun main(args: Array<String>) {
    SampleTopology.configure(args)
    TrackingServerApplication.run()
}
