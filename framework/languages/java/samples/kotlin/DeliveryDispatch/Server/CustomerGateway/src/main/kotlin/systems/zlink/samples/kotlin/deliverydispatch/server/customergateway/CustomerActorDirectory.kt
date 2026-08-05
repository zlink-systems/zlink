package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway

import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusChangedReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusNotify

class CustomerActorDirectory {
    private val gate = Any()
    private val actors = mutableMapOf<String, CustomerActor>()
    private val deliveryCustomers = mutableMapOf<String, String>()

    fun register(actor: CustomerActor) {
        synchronized(gate) {
            actors[actor.actorId()] = actor
        }
    }

    fun remove(actorId: String) {
        synchronized(gate) {
            actors.remove(actorId)
            deliveryCustomers.values.removeIf(actorId::equals)
        }
    }

    fun subscribe(customerId: String, deliveryId: String) {
        synchronized(gate) {
            deliveryCustomers[deliveryId] = customerId
        }
    }

    fun push(status: DeliveryStatusChangedReq) {
        val actor = synchronized(gate) {
            deliveryCustomers[status.deliveryId]?.let { actors[it] }
        } ?: return
        actor.push(
            DeliveryStatusNotify(
                deliveryId = status.deliveryId,
                status = status.status,
                courierId = status.courierId,
                occurredAt = status.occurredAt,
            ),
        )
    }
}
