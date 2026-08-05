package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway

import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

fun main(args: Array<String>) {
    SampleTopology.configure(args)
    CustomerGatewayApplication.run()
}
