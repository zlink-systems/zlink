package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

fun main(args: Array<String>) {
    SampleTopology.configure(args)
    DispatchServerApplication.run()
}
