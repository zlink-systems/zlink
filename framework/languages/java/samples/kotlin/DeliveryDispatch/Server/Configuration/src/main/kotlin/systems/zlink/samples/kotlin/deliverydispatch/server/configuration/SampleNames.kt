package systems.zlink.samples.kotlin.deliverydispatch.server.configuration

object SampleNames {
    const val CourierChannel: String = "deliverydispatch.courier"

    /**
     * Where the courier's decision comes back to. The offer goes out one-way and the decision
     * returns as its own one-way message, so dispatch has to be reachable as a channel server
     * (common sample spec section 7.4).
     */
    const val DispatchChannel: String = "deliverydispatch.dispatch"
    const val TrackingChannel: String = "deliverydispatch.tracking"
    const val CustomerSpotMesh: String = "delivery-customers"
    const val CourierSpotMesh: String = "delivery-couriers"
    const val CustomerStreamNode: String = "deliverydispatch.customer.stream"
    const val CourierStreamNode: String = "deliverydispatch.courier.stream"
    const val CustomerActorType: String = "deliverydispatch.customer.actor"
    const val CourierActorType: String = "deliverydispatch.courier.actor"

    const val RegistryRole: String = "registry"
    const val DispatchRole: String = "dispatch"
    const val CourierSessionRole: String = "couriersession"
    const val CourierSpotNodeRolePrefix: String = "courierspotnode"
    const val TrackingRole: String = "tracking"
    const val CustomerGatewayRole: String = "customergateway"

    const val TopologyReadyMarker: String = "topology=ready"
    const val ReassignmentMarker: String = "deliverydispatch-reassignment=completed"
    const val ServerEvidenceMarker: String = "deliverydispatch-server-evidence=completed"
    const val CompletedMarker: String = "deliverydispatch=completed"
}
