package systems.zlink.samples.kotlin.zoneworld.server.ops

import java.util.concurrent.Flow
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot
import systems.zlink.framework.monitoring.ZLinkObservedStatus
import systems.zlink.framework.monitoring.ZLinkPeerState
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeRegistry
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec

/**
 * Whether a zone node is registered and its link is up. Neither is a question the console
 * can ask: a node that has stopped is not there to answer, so the console learns it from
 * the mesh runtime it already observes in its own process.
 */
class NodeLivenessObserver(
    private val runtime: ZLinkRouteMeshRuntime,
    private val registry: NodeRegistry,
) : ApplicationRunner {
    // The runtime signals its observation publisher through a weak reference, so the
    // stream stops as soon as the publisher is collected. The console watches node
    // lifecycles for as long as it runs, so it keeps the publisher for that long.
    private var observation: Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>? = null

    override fun run(args: ApplicationArguments) {
        val publisher = runtime.observe(ZoneWorldNames.MESH, 32)
        observation = publisher
        publisher.subscribe(
            object : Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> {
                override fun onSubscribe(subscription: Flow.Subscription) {
                    subscription.request(Long.MAX_VALUE)
                }

                override fun onNext(observed: ZLinkObservedStatus<ZLinkMeshNodeSnapshot>) {
                    registry.applyLivePeers(zoneNodes(observed.status()))
                }

                override fun onError(error: Throwable) {
                    println(
                        "node observation error mesh=${ZoneWorldNames.MESH}" +
                            " detail=${error.message}",
                    )
                }

                override fun onComplete() {
                }
            },
        )
    }

    private fun zoneNodes(status: ZLinkMeshNodeSnapshot): Map<String, String> {
        val observed = LinkedHashMap<String, String>()
        status.peers()
            .filter { it.state() == ZLinkPeerState.READY }
            .forEach { peer ->
                val routingId = peer.nodeRid().toString()
                val identity = ZoneWorldNames.identityOfRoutingId(routingId)
                if (identity != null && ZoneWorldSpec.zonesOf(identity).isNotEmpty()) {
                    observed[identity] = routingId
                }
            }
        return observed
    }
}
