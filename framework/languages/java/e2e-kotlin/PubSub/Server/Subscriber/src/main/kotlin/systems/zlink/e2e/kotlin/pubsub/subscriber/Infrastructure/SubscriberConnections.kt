package systems.zlink.e2e.kotlin.pubsub.subscriber

import systems.zlink.framework.configuration.ZLinkEndpointConnections

class SubscriberConnections {
    @Volatile
    private var delegate: ZLinkEndpointConnections? = null

    fun install(value: ZLinkEndpointConnections) {
        delegate = value
    }

    fun connect(endpoint: String) {
        requireDelegate().connect(endpoint)
    }

    fun disconnect(endpoint: String) {
        requireDelegate().disconnect(endpoint)
    }

    fun list(): List<String> = requireDelegate().listConnections()

    private fun requireDelegate(): ZLinkEndpointConnections =
        delegate ?: throw IllegalStateException("subscriber connections are not initialized")
}
