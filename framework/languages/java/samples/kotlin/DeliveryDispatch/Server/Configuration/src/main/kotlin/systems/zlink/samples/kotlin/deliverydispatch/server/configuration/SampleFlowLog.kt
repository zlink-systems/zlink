package systems.zlink.samples.kotlin.deliverydispatch.server.configuration

object SampleFlowLog {
    fun roleReady(role: String, port: Int) {
        println("deliverydispatch role-ready role=$role port=$port")
    }

    fun deliveryAccepted(deliveryId: String, customerId: String) {
        println("deliverydispatch dispatch: accepted delivery=$deliveryId customer=$customerId")
    }

    fun statusRecorded(deliveryId: String, status: String, courierId: String) {
        println("deliverydispatch tracking: status delivery=$deliveryId status=$status courier=$courierId")
    }

    fun customerSubscribed(deliveryId: String) {
        println("deliverydispatch customer-session: subscribed delivery=$deliveryId")
    }
}
